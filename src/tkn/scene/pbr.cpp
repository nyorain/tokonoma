#include <tkn/scene/pbr.hpp>
#include <tkn/bits.hpp>
#include <tkn/render.hpp>
#include <tkn/types.hpp>

#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <vpp/debug.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/image.hpp>
#include <dlg/dlg.hpp>

#include <shaders/tkn.irradiance.comp.h>
#include <shaders/tkn.equirectToCube.comp.h>
#include <shaders/tkn.brdflut.comp.h>
#include <shaders/tkn.convolute.comp.h>
#include <shaders/tkn.shProj.comp.h>

namespace tkn {
namespace {

vpp::ViewableImageCreateInfo cubemapCreateInfo(nytl::Vec2ui size) {
	auto format = vk::Format::r16g16b16a16Sfloat;
	auto usage = vk::ImageUsageBits::storage |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::sampled;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, {size.x, size.y}, usage);
	info.img.flags = vk::ImageCreateBits::cubeCompatible;
	info.img.arrayLayers = 6u;
	info.view.subresourceRange.layerCount = 6u;
	info.view.viewType = vk::ImageViewType::cube;
	return info;
}

// TODO: not sure why slightly different to camera.cpp (cubeProjectionVP)
// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
// the directions vectors are like table 26 (chapter 15.6.4) in the vulkan
// spec, except that the y axis is always the opposite, since up
// is (0, 1, 0) for us, not (0, -1, 0) as it is per default in vulkan.
constexpr struct CubeFace {
	nytl::Vec3f x;
	u32 face; // id of the face
	nytl::Vec3f y;
	float roughness; // only relevant for specular filtering
	nytl::Vec3f z; // direction of the face
} faces[] = {
	{{0, 0, -1}, 0u, {0, 1, 0}, 0.f, {1, 0, 0}},
	{{0, 0, 1}, 1u, {0, 1, 0}, 0.f, {-1, 0, 0}},
	{{1, 0, 0}, 2u, {0, 0, 1}, 0.f, {0, -1, 0}},
	{{1, 0, 0}, 3u, {0, 0, -1}, 0.f, {0, 1, 0}},
	{{1, 0, 0}, 4u, {0, 1, 0}, 0.f, {0, 0, 1}},
	{{-1, 0, 0}, 5u, {0, 1, 0}, 0.f, {0, 0, -1}},
};

} // anon namespace

using namespace tkn::types;

// Cubemapper
void Cubemapper::init(vpp::DeviceMemoryAllocator& alloc,
		const nytl::Vec2ui& faceSize, vk::Sampler linear) {
	size_ = faceSize;
	auto& dev = alloc.device();

	// cubemap image
	auto memBits = dev.deviceMemoryTypes();
	auto info = cubemapCreateInfo(size_);
	dlg_assert(vpp::supported(dev, info.img));
	cubemap_ = {initCubemap_, alloc, info.img, memBits};
	vpp::nameHandle(cubemap_.image(), "Cubemapper:cubemap");

	// layouts
	auto bindings = std::array {
		vpp::descriptorBinding( // output image, irradiance
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // environment map
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &linear),
	};

	dsLayout_.init(dev, bindings);

	vk::PushConstantRange pcr{vk::ShaderStageBits::compute, 0, sizeof(CubeFace)};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};

	std::array<vk::SpecializationMapEntry, 2> entries = {{
		{0, 0, 4u},
		{1, 4u, 4u},
	}};

	std::byte constData[8u];
	auto span = nytl::Span<std::byte>(constData);
	tkn::write(span, u32(groupDimSize));
	tkn::write(span, u32(groupDimSize));

	vk::SpecializationInfo spec;
	spec.dataSize = sizeof(constData);
	spec.pData = constData;
	spec.mapEntryCount = entries.size();
	spec.pMapEntries = entries.data();

	vpp::ShaderModule shader(dev, tkn_equirectToCube_comp_data);
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

void Cubemapper::record(vk::CommandBuffer cb, vk::ImageView equirect) {
	auto info = cubemapCreateInfo(size_);
	cubemap_.init(initCubemap_, info.view);
	ds_.init(initDs_);

	// update ds
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.storage(cubemap_.vkImageView());
	dsu.imageSampler(equirect);
	dsu.apply();

	// record
	vk::ImageMemoryBarrier barrier;
	barrier.image = cubemap_.image();
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
	auto sx = std::ceil(size_.x / float(groupDimSize));
	auto sy = std::ceil(size_.y / float(groupDimSize));
	for(auto i = 0u; i < 6; ++i) {
		vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute,
			0u, sizeof(CubeFace), &faces[i]);
		vk::cmdDispatch(cb, sx, sy, 1);
	}
}

vpp::ViewableImage Cubemapper::finish() {
	auto ret = std::move(cubemap_);
	return ret;
}

// Irradiancer
void Irradiancer::init(vpp::DeviceMemoryAllocator& alloc,
		const nytl::Vec2ui& size, vk::Sampler linear, float sampleDelta) {
	size_ = size;
	auto& dev = alloc.device();

	// irradiance image
	auto memBits = dev.deviceMemoryTypes();
	auto info = cubemapCreateInfo(size_);
	dlg_assert(vpp::supported(dev, info.img));
	irradiance_ = {initIrradiance_, alloc, info.img, memBits};
	vpp::nameHandle(irradiance_.image(), "Irradiancer:irradiance");

	// init pipeline
	auto bindings = std::array {
		vpp::descriptorBinding( // output image, irradiance
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // environment cube map
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &linear),
	};

	dsLayout_.init(dev, bindings);
	vk::PushConstantRange pcr{vk::ShaderStageBits::compute, 0, sizeof(CubeFace)};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};

	std::array<vk::SpecializationMapEntry,3 > entries = {{
		{0, 0, 4u},
		{1, 4u, 4u},
		{2, 8u, 4u},
	}};

	std::byte constData[12u];
	auto span = nytl::Span<std::byte>(constData);
	tkn::write(span, u32(groupDimSize));
	tkn::write(span, u32(groupDimSize));
	tkn::write(span, float(sampleDelta));

	vk::SpecializationInfo spec;
	spec.dataSize = sizeof(constData);
	spec.pData = constData;
	spec.mapEntryCount = entries.size();
	spec.pMapEntries = entries.data();

	vpp::ShaderModule shader(dev, tkn_irradiance_comp_data);
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
	auto info = cubemapCreateInfo(size_);
	irradiance_.init(initIrradiance_, info.view);
	ds_.init(initDs_);

	// update ds
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.storage(irradiance_.vkImageView());
	dsu.imageSampler(envMap);
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
	auto sx = std::ceil(size_.x / float(groupDimSize));
	auto sy = std::ceil(size_.y / float(groupDimSize));
	for(auto i = 0u; i < 6; ++i) {
		vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute,
			0u, sizeof(CubeFace), &faces[i]);
		vk::cmdDispatch(cb, sx, sy, 1);
	}
}

vpp::ViewableImage Irradiancer::finish() {
	auto ret = std::move(irradiance_);
	return ret;
}

// EnvironmentMapFilter
void EnvironmentMapFilter::create(const vpp::Device& dev, vk::Sampler linear,
		unsigned sampleCount) {
	auto bindings = std::array {
		vpp::descriptorBinding( // output image, convolution
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // environment cube map
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &linear),
	};

	dsLayout_.init(dev, bindings);
	vk::PushConstantRange pcr{vk::ShaderStageBits::compute, 0, sizeof(CubeFace)};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};

	std::array<vk::SpecializationMapEntry, 3> entries = {{
		{0, 0, 4u},
		{1, 4u, 4u},
		{2, 8u, 4u},
	}};

	std::byte constData[12u];
	auto span = nytl::Span<std::byte>(constData);
	tkn::write(span, u32(groupDimSize));
	tkn::write(span, u32(groupDimSize));
	tkn::write(span, u32(sampleCount));

	vk::SpecializationInfo spec;
	spec.dataSize = sizeof(constData);
	spec.pData = constData;
	spec.mapEntryCount = entries.size();
	spec.pMapEntries = entries.data();

	vpp::ShaderModule shader(dev, tkn_convolute_comp_data);
	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.pSpecializationInfo = &spec;
	cpi.stage.module = shader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";

	vk::Pipeline vkpipe;
	vk::createComputePipelines(dev, {}, 1u, cpi, nullptr, vkpipe);
	pipe_ = {dev, vkpipe};
}

std::vector<EnvironmentMapFilter::Mip> EnvironmentMapFilter::record(
		vk::CommandBuffer cb, vk::ImageView cubemap, vk::Image filtered,
		unsigned mipLevels, nytl::Vec2ui size, unsigned baseLayer) {

	std::vector<Mip> mips;
	auto& dev = pipe_.device();

	// record
	mips.reserve(mipLevels - 1);
	vk::ImageViewCreateInfo ivi;
	ivi.format = vk::Format::r16g16b16a16Sfloat;
	ivi.image = filtered;
	ivi.viewType = vk::ImageViewType::cube;
	ivi.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	ivi.subresourceRange.baseArrayLayer = baseLayer;
	ivi.subresourceRange.layerCount = 6u;
	ivi.subresourceRange.levelCount = 1u;

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	for(auto m = 0u; m < mipLevels; ++m) {
		nytl::Vec2ui msize;
		msize.x = std::max(size.x >> m, 1u);
		msize.y = std::max(size.y >> m, 1u);
		unsigned dx = std::ceil(msize.x / groupDimSize);
		unsigned dy = std::ceil(msize.y / groupDimSize);

		for(auto f = 0u; f < 6u; ++f) {
			auto face = faces[f];
			face.roughness = float(m) / mipLevels;
			vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute,
				0u, sizeof(face), &face);

			auto& mip = mips.emplace_back();

			ivi.subresourceRange.baseMipLevel = m;
			mip.view = {dev, ivi};

			mip.ds = {dev.descriptorAllocator(), dsLayout_};
			vpp::DescriptorSetUpdate dsu(mip.ds);
			dsu.storage(mip.view);
			dsu.imageSampler(cubemap);
			dsu.apply();

			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
				pipeLayout_, 0, {{mip.ds.vkHandle()}}, {});
			vk::cmdDispatch(cb, dx, dy, 1);
		}
	}

	return mips;
}

// SHProjector
void SHProjector::create(const vpp::Device& dev, vk::Sampler linear) {
	auto bindings = {
		vpp::descriptorBinding( // sampled input irradiance cube
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &linear),
		vpp::descriptorBinding( // coeffs storage buffer
			vk::DescriptorType::storageBuffer,
			vk::ShaderStageBits::compute)
	};

	dsLayout_.init(dev, bindings);
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
	vpp::nameHandle(dsLayout_, "SHProjector:dsLayout");
	vpp::nameHandle(pipeLayout_, "SHProjector:pipeLayout");

	vpp::ShaderModule shader(dev, tkn_shProj_comp_data);
	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.module = shader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	pipe_ = {dev, cpi};
	vpp::nameHandle(pipe_, "SHProjector:pipe");

	// TODO(perf): defer creation of ds and buffer
	ds_ = {dev.descriptorAllocator(), dsLayout_};
	vpp::nameHandle(ds_, "SHProjector:ds");

	// TODO: allow to configure whether to allocate on host or
	// device memory
	dst_ = {dev.bufferAllocator(), sizeof(nytl::Vec3f) * 9,
		vk::BufferUsageBits::storageBuffer, dev.hostMemoryTypes()};
}

void SHProjector::record(vk::CommandBuffer cb, vk::ImageView irradianceCube) {
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler(irradianceCube);
	dsu.storage(dst_);
	dsu.apply();

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {ds_});
	vk::cmdDispatch(cb, 1, 1, 1);
}

} // namespace tkn
