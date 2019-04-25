#include "skybox.hpp"
#include <stage/bits.hpp>

#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>

#include <shaders/skybox.vert.h>
#include <shaders/skybox.frag.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop

// TODO: allow loading panorama data
// https://stackoverflow.com/questions/29678510/convert-21-equirectangular-panorama-to-cube-map
// load cubemap

// XXX: the way vulkan handles cubemap samplers and image coorindates,
// top and bottom usually have to be rotated 180 degrees (or at least
// from the skybox set i tested with, see /assets/skyboxset1, those
// were rotated manually to fit)
void Skybox::init(vpp::Device& dev, vk::RenderPass rp,
		vk::SampleCountBits samples) {
	// ds layout
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex),
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment),
	};

	dsLayout_ = {dev, bindings};
	pipeLayout_ = {dev, {dsLayout_}, {}};

	vpp::ShaderModule vertShader(dev, skybox_vert_data);
	vpp::ShaderModule fragShader(dev, skybox_frag_data);
	vpp::GraphicsPipelineInfo gpi {rp, pipeLayout_, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}, 0, samples};

	// depth test disabled by default
	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	// gpi.rasterization.cullMode = vk::CullModeBits::back;
	// gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), NULL, vkpipe);
	pipe_ = {dev, vkpipe};

	// indices
	indices_ = {dev.bufferAllocator(), 36u * sizeof(std::uint16_t),
		vk::BufferUsageBits::indexBuffer | vk::BufferUsageBits::transferDst,
		0, dev.deviceMemoryTypes()};
	std::array<std::uint16_t, 36> indices = {
		0, 1, 2,  2, 1, 3, // front
		1, 5, 3,  3, 5, 7, // right
		2, 3, 6,  6, 3, 7, // top
		4, 0, 6,  6, 0, 2, // left
		4, 5, 0,  0, 5, 1, // bottom
		5, 4, 7,  7, 4, 6, // back
	};
	vpp::writeStaging430(indices_, vpp::raw(*indices.data(), 36u));

	auto base = std::string("../assets/skyboxset1/SunSet/SunSet");
	// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
	const char* names[] = {
		"Right",
		"Left",
		"Up",
		"Down",
		"Back",
		"Front",
	};
	auto suffix = "2048.png";

	auto width = 0u;
	auto height = 0u;

	std::array<const std::byte*, 6> faceData {};

	for(auto i = 0u; i < 6u; ++i) {
		auto filename = base + names[i] + suffix;
		int iwidth, iheight, channels;
		unsigned char* data = stbi_load(filename.c_str(), &iwidth, &iheight,
			&channels, 4);
		if(!data) {
			dlg_warn("Failed to open texture file {}", filename);

			std::string err = "Could not load image from ";
			err += filename;
			err += ": ";
			err += stbi_failure_reason();
			throw std::runtime_error(err);
		}

		dlg_assert(iwidth > 0 && iheight > 0);
		if(width == 0 || height == 0) {
			width = iwidth;
			height = iheight;
			dlg_info("skybox size: {} {}", width, height);
		} else {
			dlg_assert(int(width) == iwidth);
			dlg_assert(int(height) == iheight);
		}

		faceData[i] = reinterpret_cast<const std::byte*>(data);
	}

	// free data
	nytl::ScopeGuard guard([&]{
		for(auto i = 0u; i < 6u; ++i) {
			std::free(const_cast<std::byte*>(faceData[i]));
		}
	});

	auto imgi = vpp::ViewableImageCreateInfo::color(
		dev, vk::Extent3D {width, height, 1u}).value();
	imgi.img.arrayLayers = 6u;
	imgi.img.flags = vk::ImageCreateBits::cubeCompatible;
	imgi.view.viewType = vk::ImageViewType::cube;
	imgi.view.subresourceRange.layerCount = 6u;
	cubemap_ = {dev, imgi};

	// buffer
	// XXX: this might get large
	auto totalSize = 6u * width * height * 4u;
	auto stage = vpp::SubBuffer(dev.bufferAllocator(), totalSize,
		vk::BufferUsageBits::transferSrc, 0u, dev.hostMemoryTypes());

	auto map = stage.memoryMap();
	auto span = map.span();
	auto offset = stage.offset();
	std::array<vk::BufferImageCopy, 6u> copies {};
	for(auto i = 0u; i < 6u; ++i) {
		copies[i] = {};
		copies[i].bufferOffset = offset;
		copies[i].imageExtent = {width, height, 1u};
		copies[i].imageOffset = {};
		copies[i].imageSubresource.aspectMask = vk::ImageAspectBits::color;
		copies[i].imageSubresource.baseArrayLayer = i;
		copies[i].imageSubresource.layerCount = 1u;
		copies[i].imageSubresource.mipLevel = 0u;
		offset += width * height * 4u;
		doi::write(span, faceData[i], width * height * 4u);
	}

	map = {};

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});
	vpp::changeLayout(cb, cubemap_.image(), vk::ImageLayout::undefined,
		vk::PipelineStageBits::topOfPipe, {}, vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferWrite,
		{vk::ImageAspectBits::color, 0, 1, 0, 6u});
	vk::cmdCopyBufferToImage(cb, stage.buffer(), cubemap_.image(),
		vk::ImageLayout::transferDstOptimal, copies);
	vpp::changeLayout(cb, cubemap_.image(), vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferWrite,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::fragmentShader,
		vk::AccessBits::shaderRead,
		{vk::ImageAspectBits::color, 0, 1, 0, 6u});
	vk::endCommandBuffer(cb);

	vk::SubmitInfo si {};
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb.vkHandle();
	qs.wait(qs.add(si));

	// ubo
	auto uboSize = sizeof(nytl::Mat4f);
	ubo_ = {dev.bufferAllocator(), uboSize, vk::BufferUsageBits::uniformBuffer,
		0u, dev.hostMemoryTypes()};

	// sampler
	vk::SamplerCreateInfo sci {};
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.minLod = 0.0;
	sci.maxLod = 0.25;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	sampler_ = {dev, sci};

	// ds
	ds_ = {dev.descriptorAllocator(), dsLayout_};
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	dsu.imageSampler({{sampler_, cubemap_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.apply();
}

void Skybox::render(vk::CommandBuffer cb) {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pipeLayout_, 0, {ds_}, {});
	vk::cmdBindIndexBuffer(cb, indices_.buffer(),
		indices_.offset(), vk::IndexType::uint16);
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
}

void Skybox::updateDevice(const nytl::Mat4f& viewProj) {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, viewProj);
}
