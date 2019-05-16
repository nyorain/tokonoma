#include <stage/scene/pbr.hpp>
#include <stage/bits.hpp>
#include <stage/types.hpp>

#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <vpp/debug.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/image.hpp>
#include <dlg/dlg.hpp>

#include <shaders/stage.irradiance.comp.h>
#include <shaders/stage.brdflut.comp.h>

namespace doi {
namespace {

vpp::ViewableImageCreateInfo irradianceCreateInfo(nytl::Vec2ui size) {
	auto format = vk::Format::r16g16b16a16Sfloat;
	auto usage = vk::ImageUsageBits::storage |
		vk::ImageUsageBits::sampled;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, {size.x, size.y}, usage);
	info.img.flags = vk::ImageCreateBits::cubeCompatible;
	info.img.arrayLayers = 6u;
	info.view.subresourceRange.layerCount = 6u;
	info.view.viewType = vk::ImageViewType::cube;
	return info;
}

// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
// NOTE: mainly through testing...
constexpr struct {
	nytl::Vec4f x;
	nytl::Vec4f y;
	nytl::Vec4f z;
} faces[] = {
	{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
	{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}},
	{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
	{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
	{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
	{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
};

} // anon namespace

using namespace doi::types;

// Irradiancer
void Irradiancer::init(vpp::DeviceMemoryAllocator& alloc,
		const nytl::Vec2ui& size, vk::Sampler linear, float sampleDelta) {
	size_ = size;
	auto& dev = alloc.device();

	// irradiance
	auto memBits = dev.deviceMemoryTypes();
	auto info = irradianceCreateInfo(size_);
	dlg_assert(vpp::supported(dev, info.img));
	irradiance_ = {initIrradiance_, dev, info.img, memBits, &alloc};
	vpp::nameHandle(irradiance_.image(), "Irradiancer:irradiance");

	// init pipeline
	auto bindings = {
		vpp::descriptorBinding( // output image, irradiance
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // environment map
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &linear),
	};

	dsLayout_ = {dev, bindings};

	std::uint32_t pcrSize = sizeof(nytl::Vec4f) * 3;
	vk::PushConstantRange pcr{vk::ShaderStageBits::compute, 0, pcrSize};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};

	std::array<vk::SpecializationMapEntry,3 > entries = {{
		{0, 0, 4u},
		{1, 4u, 4u},
		{2, 8u, 4u},
	}};

	std::byte constData[12u];
	auto span = nytl::Span<std::byte>(constData);
	doi::write(span, u32(groupDimSize));
	doi::write(span, u32(groupDimSize));
	doi::write(span, float(sampleDelta));

	vk::SpecializationInfo spec;
	spec.dataSize = sizeof(constData);
	spec.pData = constData;
	spec.mapEntryCount = entries.size();
	spec.pMapEntries = entries.data();

	vpp::ShaderModule shader(dev, stage_irradiance_comp_data);
	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.pSpecializationInfo = &spec;
	cpi.stage.module = shader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";

	vk::Pipeline vkpipe;
	vk::createComputePipelines(dev, {}, 1u, cpi, nullptr, vkpipe);
	pipe_ = {dev, vkpipe};

	// ds
	ds_ = {initDs_, dev.descriptorAllocator(), dsLayout_};
}

void Irradiancer::record(vk::CommandBuffer cb, vk::ImageView envMap) {
	auto info = irradianceCreateInfo(size_);
	irradiance_.init(initIrradiance_, info.view);
	ds_.init(initDs_);

	// update ds
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.storage({{{}, irradiance_.vkImageView(), vk::ImageLayout::general}});
	dsu.imageSampler({{{}, envMap, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.apply();

	// record
	vk::ImageMemoryBarrier barrier;
	barrier.image = irradiance_.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 6};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {{ds_.vkHandle()}}, {});
	auto pcrSize = sizeof(nytl::Vec4f) * 3;
	auto sx = std::ceil(size_.x / float(groupDimSize));
	auto sy = std::ceil(size_.y / float(groupDimSize));
	for(auto i = 0u; i < 6; ++i) {
		vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute,
			0u, pcrSize, &faces[i]);
		vk::cmdDispatch(cb, sx, sy, 1);
	}

	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	barrier.dstAccessMask = vk::AccessBits::shaderRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::bottomOfPipe, {}, {}, {}, {{barrier}});
}

vpp::ViewableImage Irradiancer::finish() {
	auto ret = std::move(irradiance_);
	*this = {};
	return ret;
}

// EnvironMapFilter

} // namespace doi
