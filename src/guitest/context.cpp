#include "context.hpp"
#include "scene.hpp"
#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/commandAllocator.hpp>
#include <dlg/dlg.hpp>

#include <shaders/guitest.fill2.vert.h>
#include <shaders/guitest.fill2.frag.h>

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

	// query properties
	numBindableTextures_ = 15u; // TODO!

	// dummies
	dummyBuffer_ = {bufferAllocator(), 4u,
		vk::BufferUsageBits::storageBuffer, dev.deviceMemoryTypes()};

	auto imgInfo = vpp::ViewableImageCreateInfo(vk::Format::r8g8b8a8Unorm,
		vk::ImageAspectBits::color, {1, 1}, vk::ImageUsageBits::sampled);
	dummyImage_ = {devMemAllocator(), imgInfo};

	// perpare layouts
	// TODO: fill dummy image and buffer?
	auto cb = recordableUploadCmdBuf();
	vk::ImageMemoryBarrier barrier;
	barrier.image = dummyImage_.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	barrier.dstAccessMask = vk::AccessBits::shaderRead;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::allGraphics, {}, {}, {}, {{barrier}});

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
	dsLayout_.init(dev, bindings);

	// pipeLayout
	std::vector<vk::PushConstantRange> pcrs;

	if(!multidrawIndirect()) {
		auto& pcr = pcrs.emplace_back();

		// See scene.cpp and fill.vert.
		// We need this additional push constant range to pass an equivalent
		// of gl_DrawID
		pcr.offset = 0u;
		pcr.size = 4u;
		pcr.stageFlags = stages;
	}

	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, pcrs};

	// pipeline
	// TODO: select shaders based on supported features
	vpp::ShaderModule vert(dev, guitest_fill2_vert_data);
	vpp::ShaderModule frag(dev, guitest_fill2_frag_data);

	// TODO: specialize numTextures

	vpp::GraphicsPipelineInfo gpi(settings_.renderPass, pipeLayout_, {{{
		{vert, vk::ShaderStageBits::vertex},
		{frag, vk::ShaderStageBits::fragment}
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

vpp::BufferAllocator& Context::bufferAllocator() const {
	return device().bufferAllocator();
}

vpp::DescriptorAllocator& Context::dsAllocator() const {
	return device().descriptorAllocator();
}

vpp::DeviceMemoryAllocator& Context::devMemAllocator() const {
	return device().devMemAllocator();
}

void Context::keepAlive(vpp::SubBuffer buf) {
	keepAlive_.bufs.emplace_back(std::move(buf));
}

void Context::keepAlive(vpp::ViewableImage img) {
	keepAlive_.imgs.emplace_back(std::move(img));
}

} // namespace rvg2
