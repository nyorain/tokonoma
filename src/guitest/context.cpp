#include "context.hpp"
#include "scene.hpp"
#include "paint.hpp"
#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/commandAllocator.hpp>
#include <dlg/dlg.hpp>

#include <shaders/guitest.fill2.vert.h>
#include <shaders/guitest.fill2.multidraw.vert.h>
#include <shaders/guitest.fill2.clip.vert.h>
#include <shaders/guitest.fill2.multidraw.clip.vert.h>
#include <shaders/guitest.fill2.frag.h>
#include <shaders/guitest.fill2.clip.frag.h>

namespace vk {

// HACK: this shouldn't be here. But vkpp generation for this function
// is mess, the last parameter is inout instead of only out.
inline void getPhysicalDeviceProperties2(
		PhysicalDevice physicalDevice,
		PhysicalDeviceProperties2& ret,
		DynamicDispatch* dispatcher = nullptr) {

	VKPP_DISPATCH_GLOBAL(dispatcher,
		vkGetPhysicalDeviceProperties2,
		(VkPhysicalDevice)(physicalDevice),
		(VkPhysicalDeviceProperties2*)(&ret));
}

} // namespace vk

namespace rvg2 {

Context::Context(vpp::Device& dev, const ContextSettings& settings) :
		dev_(dev), settings_(settings) {

	if(!(settings_.deviceFeatures & DeviceFeature::uniformDynamicArrayIndexing)) {
		dlg_error("rvg::Context: deviceFeatures MUST include uniformDynamicArrayIndexing");
		throw std::runtime_error("rvg::Context: deviceFeatures MUST include uniformDynamicArrayIndexing");
	}

	if(settings_.samples == vk::SampleCountBits{}) {
		settings_.samples = vk::SampleCountBits::e1;
	}

	uploadCb_ = dev.commandAllocator().get(settings_.uploadQueueFamily,
		vk::CommandPoolCreateBits::resetCommandBuffer);
	uploadSemaphore_ = vpp::Semaphore(dev);

	// dummies
	struct {
		nytl::Mat4f mat;
		PaintData data;
	} bufData = {
		nytl::identity<4, float>(),
		colorPaint({1.f, 0.f, 1.f, 1.f}),
	};

	dummyBuffer_ = {bufferAllocator(), sizeof(bufData),
		vk::BufferUsageBits::storageBuffer | vk::BufferUsageBits::transferDst,
		dev.deviceMemoryTypes()};

	auto imgInfo = vpp::ViewableImageCreateInfo(vk::Format::r8g8b8a8Unorm,
		vk::ImageAspectBits::color, {1, 1}, vk::ImageUsageBits::sampled);
	dummyImage_ = {devMemAllocator(), imgInfo};

	// prepare layouts
	// TODO: fill dummy image with white or something?
	auto cb = recordableUploadCmdBuf();
	vk::ImageMemoryBarrier barrier;
	barrier.image = dummyImage_.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	barrier.dstAccessMask = vk::AccessBits::shaderRead;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::allGraphics, {}, {}, {}, {{barrier}});

	vk::cmdUpdateBuffer(cb, dummyBuffer_.buffer(), dummyBuffer_.offset(),
		sizeof(bufData), &bufData);

	// dsLayout
	// sampler
	vk::SamplerCreateInfo samplerInfo {};
	samplerInfo.magFilter = vk::Filter::linear;
	samplerInfo.minFilter = vk::Filter::linear;
	samplerInfo.minLod = 0.f;
	samplerInfo.maxLod = 0.25f;
	samplerInfo.maxAnisotropy = 1.f;
	samplerInfo.mipmapMode = vk::SamplerMipmapMode::nearest;
	samplerInfo.addressModeU = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.addressModeV = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.addressModeW = vk::SamplerAddressMode::clampToEdge;
	samplerInfo.borderColor = vk::BorderColor::floatOpaqueWhite;
	sampler_ = {dev, samplerInfo};

	// figure out limits
	constexpr auto nonTextureResources = 10u;
	vk::DescriptorPoolCreateFlags dsAllocFlags {};
	if(settings_.descriptorIndexingFeatures) {
		descriptorIndexing_ = std::make_unique<vk::PhysicalDeviceDescriptorIndexingFeaturesEXT>(*settings_.descriptorIndexingFeatures);
		dsAllocFlags |= vk::DescriptorPoolCreateBits::updateAfterBindEXT;

		if(descriptorIndexing_->descriptorBindingSampledImageUpdateAfterBind) {
			vk::PhysicalDeviceDescriptorIndexingPropertiesEXT indexingProps;
			vk::PhysicalDeviceProperties2 props;
			props.pNext = &indexingProps;

			vk::getPhysicalDeviceProperties2(device().vkPhysicalDevice(), props);

			auto m = indexingProps.maxPerStageUpdateAfterBindResources;
			m = std::min(m, props.properties.limits.maxPerStageResources - nonTextureResources);
			m = std::min(m, indexingProps.maxPerStageDescriptorUpdateAfterBindSampledImages);
			m = std::min(m, indexingProps.maxPerStageDescriptorUpdateAfterBindSamplers);
			m = std::min(m, indexingProps.maxPerStageDescriptorUpdateAfterBindSamplers);

			numBindableTextures_ = m;
			dlg_debug("rvg numBindableTextures (indexing): {}", numBindableTextures_);
		} else {
			dlg_info("rvg can't use descriptor indexing (bindless) textures "
				"since the required descriptor indexing feature isn't enabled");
		}
	}

	if(!numBindableTextures_) {
		auto& limits = device().properties().limits;
		numBindableTextures_ = std::min(
			limits.maxPerStageDescriptorSampledImages,
			std::min(
				limits.maxPerStageResources - nonTextureResources,
				limits.maxPerStageDescriptorSamplers));
		dlg_debug("rvg numBindableTextures: {}", numBindableTextures_);
		// the spec requires this
		dlg_assert(numBindableTextures_ >= 16 - nonTextureResources);
	}

	// TODO: own, custom limit. Replace that with using partially bound
	// descriptors if supported by device.
	numBindableTextures_ = std::min(numBindableTextures_, 4 * 1024u);
	// We need to subtract once for the font atlas
	--numBindableTextures_;

	dsAlloc_.init(dev, dsAllocFlags);

	const auto stages = vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment;
	auto bindings = std::array{
		// clip
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// transform
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// paint, buffer + textures
		// don't bind samplers here since this may potentially be *a lot* of
		// textures
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler, stages,
			nullptr, numBindableTextures()),
		// draw commands
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// font atlas. Use our linear sampler
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler, stages,
			&sampler_.vkHandle()),
	};

	// auto number them
	unsigned int highestBinding = 0u;
	for(auto& binding : bindings) {
		auto& bid = binding.binding;
		if(bid == vpp::autoDescriptorBinding) {
			bid = highestBinding++;
		} else {
			highestBinding = std::max(highestBinding, bid + 1);
		}
	}

	vk::DescriptorSetLayoutCreateInfo dslc;
	dslc.bindingCount = bindings.size();
	dslc.pBindings = bindings.data();

	std::array<vk::DescriptorBindingFlagsEXT, 6> bindingFlags {};
	static_assert(bindingFlags.max_size() == bindings.max_size());

	vk::DescriptorSetLayoutBindingFlagsCreateInfoEXT bindingFlagsInfo;
	if(descriptorIndexing_) {
		dslc.flags = vk::DescriptorSetLayoutCreateBits::updateAfterBindPoolEXT;

		if(descriptorIndexing_->descriptorBindingStorageBufferUpdateAfterBind) {
			bindingFlags[0] = vk::DescriptorBindingBitsEXT::updateAfterBind;
			bindingFlags[1] = vk::DescriptorBindingBitsEXT::updateAfterBind;
			bindingFlags[2] = vk::DescriptorBindingBitsEXT::updateAfterBind;
			bindingFlags[4] = vk::DescriptorBindingBitsEXT::updateAfterBind;
		}

		// TODO: support varaible descriptor count (when device does) for textures.
		// Could also use the partially bound flag (when device supports it)
		// and not even set unused textures.
		if(descriptorIndexing_->descriptorBindingSampledImageUpdateAfterBind) {
			bindingFlags[3] = vk::DescriptorBindingBitsEXT::updateAfterBind;
			bindingFlags[5] = vk::DescriptorBindingBitsEXT::updateAfterBind;
		}

		bindingFlagsInfo.bindingCount = bindingFlags.size();
		bindingFlagsInfo.pBindingFlags = bindingFlags.data();
	}

	dsLayout_.init(dev, dslc);

	// pipeLayout
	vk::PushConstantRange pcr;
	// Members:
	// - targetSize (uvec2)
	//   We need to pass the current render target size so we can
	//   convert to ndc.
	// - cmdOffset (uint)
	//   We need this additional push constant range to pass an offset
	//   into the indirect bindings buffer.
	pcr.offset = 0u;
	pcr.size = 12u;
	pcr.stageFlags = vk::ShaderStageBits::vertex;

	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};

	// pipeline
	vpp::ShaderModule vert;
	vpp::ShaderModule frag;
	if(settings_.deviceFeatures & DeviceFeature::clipDistance) {
		if(multidrawIndirect()) {
			vert = {dev, guitest_fill2_multidraw_clip_vert_data};
		} else {
			vert = {dev, guitest_fill2_clip_vert_data};
		}

		frag = {dev, guitest_fill2_clip_frag_data};
	} else {
		if(multidrawIndirect()) {
			vert = {dev, guitest_fill2_multidraw_vert_data};
		} else {
			vert = {dev, guitest_fill2_vert_data};
		}

		frag = {dev, guitest_fill2_frag_data};
	}

	// specialize numBindableTextures
	vk::SpecializationMapEntry specEntry;
	specEntry.constantID = 0u;
	specEntry.offset = 0u;
	specEntry.size = 4u;

	u32 specData = numBindableTextures_;
	vk::SpecializationInfo spec;
	spec.pMapEntries = &specEntry;
	spec.mapEntryCount = 1u;
	spec.pData = &specData;
	spec.dataSize = 4u;

	vpp::GraphicsPipelineInfo gpi(settings_.renderPass, pipeLayout_, {{{
		{vert, vk::ShaderStageBits::vertex, &spec},
		{frag, vk::ShaderStageBits::fragment, &spec}
	}}}, settings_.subpass, settings_.samples);

	// from tkn/render.hpp
	struct PipelineVertexInfo {
		std::vector<vk::VertexInputAttributeDescription> attribs;
		std::vector<vk::VertexInputBindingDescription> bindings;

		vk::PipelineVertexInputStateCreateInfo info() const {
			vk::PipelineVertexInputStateCreateInfo ret;
			ret.pVertexAttributeDescriptions = attribs.data();
			ret.vertexAttributeDescriptionCount = attribs.size();
			ret.pVertexBindingDescriptions = bindings.data();
			ret.vertexBindingDescriptionCount = bindings.size();
			return ret;
		}
	};

	auto vertexInfo = PipelineVertexInfo{{
			{0, 0, vk::Format::r32g32Sfloat, offsetof(Vertex, pos)},
			{1, 0, vk::Format::r32g32Sfloat, offsetof(Vertex, uv)},
			{2, 0, vk::Format::r8g8b8a8Unorm, offsetof(Vertex, color)},
		}, {
			{0, sizeof(Vertex), vk::VertexInputRate::vertex},
		}
	};

	gpi.vertex = vertexInfo.info();
	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	pipe_ = {dev, gpi.info(), settings_.pipelineCache};
}

vk::Semaphore Context::endFrameSubmit(vk::SubmitInfo& si) {
	auto* cb = endFrameWork();
	if(!cb) {
		return {};
	}

	si.commandBufferCount = 1u;
	si.pCommandBuffers = cb;
	si.signalSemaphoreCount = 1u;
	si.pSignalSemaphores = &uploadSemaphore_.vkHandle();

	return uploadSemaphore_;
}

const vk::CommandBuffer* Context::endFrameWork() {
	if(!uploadWork_) {
		return nullptr;
	}

	vk::endCommandBuffer(uploadCb_);
	uploadWork_ = false;
	keepAliveLast_ = std::move(keepAlive_);
	return &uploadCb_.vkHandle();
}

vk::CommandBuffer Context::recordableUploadCmdBuf() {
	if(!uploadWork_) {
		uploadWork_ = true;
		vk::beginCommandBuffer(uploadCb_, {});
	}

	return uploadCb_;
}

vpp::BufferAllocator& Context::bufferAllocator() {
	return device().bufferAllocator();
}

vpp::DescriptorAllocator& Context::dsAllocator() {
	return dsAlloc_;
}

vpp::DeviceMemoryAllocator& Context::devMemAllocator() {
	return device().devMemAllocator();
}

void Context::keepAlive(vpp::SubBuffer buf) {
	keepAlive_.bufs.emplace_back(std::move(buf));
}

void Context::keepAlive(vpp::ViewableImage img) {
	keepAlive_.imgs.emplace_back(std::move(img));
}

vpp::BufferSpan Context::defaultTransform() const {
	return {dummyBuffer_.buffer(), sizeof(Mat4f), dummyBuffer_.offset()};
}

vpp::BufferSpan Context::defaultPaint() const {
	return {dummyBuffer_.buffer(), sizeof(PaintData),
		dummyBuffer_.offset() + sizeof(Mat4f)};
}

} // namespace rvg2
