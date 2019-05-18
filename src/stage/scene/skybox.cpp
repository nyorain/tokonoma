#include <stage/scene/skybox.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>

#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/debug.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/handles.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>

#include <shaders/stage.fullscreen.vert.h>
#include <shaders/stage.equirectToCube.frag.h>
#include <shaders/stage.skybox.vert.h>
#include <shaders/stage.skybox.frag.h>
#include <shaders/stage.irradiance.frag.h>

// implementation in texture.cpp
// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wcast-align"
// #pragma GCC diagnostic ignored "-Wunused-parameter"
// #define STB_IMAGE_IMPLEMENTATION
// #include "stb_image.h"
// #pragma GCC diagnostic pop

// TODO: allow reusing sampler (if passing in init method)
// TODO: allow passing filename/base/something...
// TODO: allow loading panorama data
// https://stackoverflow.com/questions/29678510/convert-21-equirectangular-panorama-to-cube-map
// TODO: use WorkBatcher allocators
// TODO: support deferred initialization

namespace doi {

// TODO: write a stage tool that converts hdr equirectangular to hdr
// cubemaps
void Skybox::init(const WorkBatcher& wb, nytl::StringParam hdrFile,
		vk::RenderPass rp, unsigned subpassID, vk::SampleCountBits samples) {
	auto& dev = wb.dev;

	dev_ = &dev;
	initPipeline(dev, rp, subpassID, samples);
	// TODO: maybe host visible is better here, we only read from it
	// once anyways. Add option to loadTexture to allow that
	auto params = doi::TextureCreateParams {};
	params.format = vk::Format::r16g16b16a16Sfloat;
	auto equirectImg = doi::Texture(wb, read(hdrFile, true), params);
	vpp::nameHandle(equirectImg.image(), "Skybox:equirectImg");

	// convert equirectangular to cubemap
	// renderpass
	// NOTE: we could use one renderpass with 6 attachments here (we
	// render fullscreen anyways) but vulkan doesn't require implementations
	// to support 6 color attachments, only 4 are required.
	// So don't bother, just use multiple passes.
	vk::AttachmentDescription attachment;
	attachment.finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	attachment.initialLayout = vk::ImageLayout::undefined;
	attachment.format = vk::Format::r16g16b16a16Sfloat;
	attachment.samples = vk::SampleCountBits::e1;
	attachment.loadOp = vk::AttachmentLoadOp::dontCare;
	attachment.storeOp = vk::AttachmentStoreOp::store;

	vk::AttachmentReference colRef;
	colRef.attachment = 0u;
	colRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colRef;

	// TODO, probably not correct/required like that
	vk::SubpassDependency dependency;
	dependency.srcSubpass = 0u;
	dependency.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	dependency.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	dependency.dstSubpass = vk::subpassExternal;
	dependency.dstStageMask = vk::PipelineStageBits::computeShader |
		vk::PipelineStageBits::fragmentShader |
		vk::PipelineStageBits::earlyFragmentTests |
		vk::PipelineStageBits::lateFragmentTests |
		vk::PipelineStageBits::transfer;
	dependency.dstAccessMask = vk::AccessBits::inputAttachmentRead |
		vk::AccessBits::depthStencilAttachmentRead |
		vk::AccessBits::depthStencilAttachmentWrite |
		vk::AccessBits::transferRead |
		vk::AccessBits::shaderRead;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = 1u;
	rpi.pAttachments = &attachment;
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;
	rpi.dependencyCount = 1u;
	rpi.pDependencies = &dependency;

	auto renderPass = vpp::RenderPass(dev, rpi);

	// pipeline
	// we just reuse some of the real resources for initialization, they
	// are good enough and we don't create additional resources
	std::uint32_t pcrSize = sizeof(nytl::Vec4f) * 3;
	vk::PushConstantRange pcr{vk::ShaderStageBits::fragment, 0, pcrSize};
	vpp::PipelineLayout initPipeLayout = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};

	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule fragShader(dev, stage_equirectToCube_frag_data);
	vpp::GraphicsPipelineInfo gpi {renderPass, initPipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, 0u};

	gpi.depthStencil.depthTestEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

	vk::Pipeline vkPipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), NULL, vkPipe);
	auto pipe = vpp::Pipeline(dev, vkPipe);

	// cubemap
	// TODO: make dynamic/configurable, depending on hdr size
	auto width = 2048u;
	auto height = 2048u;
	auto format = vk::Format::r16g16b16a16Sfloat;
	auto usage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::sampled;
	auto imgi = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, {width, height}, usage);
	imgi.img.flags = vk::ImageCreateBits::cubeCompatible;
	imgi.img.arrayLayers = 6u;
	imgi.view.subresourceRange.layerCount = 6u;
	imgi.view.viewType = vk::ImageViewType::cube;
	dlg_assert(vpp::supported(dev, imgi.img));
	cubemap_ = {vpp::ViewableImage{dev, imgi}};
	vpp::nameHandle(cubemap_.image(), "Skybox:cubemap");

	// ds data
	{
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		dsu.imageSampler({{{}, equirectImg.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
	}

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});
	vpp::nameHandle(cb, "Skybox:init:cb");

	// keep alive until command buffer has completed
	std::vector<vpp::ImageView> views;
	std::vector<vpp::Framebuffer> fbs;

	// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
	// NOTE: mainly through testing...
	struct {
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

	// framebuffer
	for(auto i = 0u; i < 6u; ++i) {
		vk::ImageViewCreateInfo ivi;
		ivi.format = format;
		ivi.image = cubemap_.vkImage();
		ivi.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		ivi.subresourceRange.baseArrayLayer = i;
		ivi.subresourceRange.layerCount = 1;
		ivi.subresourceRange.levelCount = 1;
		ivi.viewType = vk::ImageViewType::e2d;
		auto renderView = vpp::ImageView(dev, ivi);

		vk::FramebufferCreateInfo fbi;
		fbi.attachmentCount = 1;
		fbi.pAttachments = &renderView.vkHandle();
		fbi.width = width;
		fbi.height = height;
		fbi.layers = 1;
		fbi.renderPass = renderPass;
		auto fb = vpp::Framebuffer(dev, fbi);

		// record cb
		// TODO: requires pipeline barriers
		vk::RenderPassBeginInfo beginInfo;
		beginInfo.framebuffer = fb;
		beginInfo.renderArea.extent = {width, height};
		beginInfo.renderPass = renderPass;
		vk::cmdBeginRenderPass(cb, beginInfo, vk::SubpassContents::eInline);
		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdPushConstants(cb, initPipeLayout, vk::ShaderStageBits::fragment,
			0u, pcrSize, &faces[i]);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			initPipeLayout, 0, {{ds_.vkHandle()}}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0);

		vk::cmdEndRenderPass(cb);

		views.push_back(std::move(renderView));
		fbs.push_back(std::move(fb));
	}

	// create irradiance ===
	// irradiance is blurred as hell anyways, low res should be ok
	width = 32u;
	height = 32u;
	auto info = vpp::ViewableImageCreateInfo(format, vk::ImageAspectBits::color,
		{width, height}, usage);
	info.img.flags = vk::ImageCreateBits::cubeCompatible;
	info.img.arrayLayers = 6u;
	info.view.subresourceRange.layerCount = 6u;
	info.view.viewType = vk::ImageViewType::cube;
	dlg_assert(vpp::supported(dev, info.img));
	irradiance_ = {dev, info};

	vpp::TrDs irradianceDs(wb.alloc.ds, dsLayout_);

	// ds data
	// read from generated cubemap now
	{
		vpp::DescriptorSetUpdate dsu(irradianceDs);
		dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		dsu.imageSampler({{{}, cubemap_.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
	}

	vpp::ShaderModule irradianceShader(dev, stage_irradiance_frag_data);
	vpp::GraphicsPipelineInfo igpi{renderPass, initPipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{irradianceShader, vk::ShaderStageBits::fragment}
	}}}, 0u};

	igpi.depthStencil.depthTestEnable = false;
	igpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

	vk::createGraphicsPipelines(dev, {}, 1, igpi.info(), NULL, vkPipe);
	auto irradiancePipe = vpp::Pipeline(dev, vkPipe);

	for(auto i = 0u; i < 6u; ++i) {
		vk::ImageViewCreateInfo ivi;
		ivi.format = format;
		ivi.image = irradiance_.vkImage();
		ivi.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		ivi.subresourceRange.baseArrayLayer = i;
		ivi.subresourceRange.layerCount = 1;
		ivi.subresourceRange.levelCount = 1;
		ivi.viewType = vk::ImageViewType::e2d;
		auto renderView = vpp::ImageView(dev, ivi);

		vk::FramebufferCreateInfo fbi;
		fbi.attachmentCount = 1;
		fbi.pAttachments = &renderView.vkHandle();
		fbi.width = width;
		fbi.height = height;
		fbi.layers = 1;
		fbi.renderPass = renderPass;
		auto fb = vpp::Framebuffer(dev, fbi);

		// record cb
		vk::RenderPassBeginInfo beginInfo;
		beginInfo.framebuffer = fb;
		beginInfo.renderArea.extent = {width, height};
		beginInfo.renderPass = renderPass;
		vk::cmdBeginRenderPass(cb, beginInfo, vk::SubpassContents::eInline);
		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdPushConstants(cb, initPipeLayout, vk::ShaderStageBits::fragment,
			0u, pcrSize, &faces[i]);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, irradiancePipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			initPipeLayout, 0, {{irradianceDs.vkHandle()}}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0);

		vk::cmdEndRenderPass(cb);

		views.push_back(std::move(renderView));
		fbs.push_back(std::move(fb));
	}

	// wait for cb
	// when supporting deferred inialization (and using the batcher cb)
	// we have to make sure all framebuffers etc stay alive
	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));

	// set real ds data
	writeDs();
}

// XXX: the way vulkan handles cubemap samplers and image coorindates,
// top and bottom usually have to be rotated 180 degrees (or at least
// from the skybox set i tested with, see /assets/skyboxset1, those
// were rotated manually to fit)
void Skybox::init(const WorkBatcher& wb, vk::RenderPass rp,
		unsigned subpass, vk::SampleCountBits samples) {
	auto& dev = wb.dev;
	initPipeline(dev, rp, subpass, samples);

	// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
	auto base = std::string("../assets/skyboxset1/SunSet");
	auto suffix = std::string("2048.png");
	std::string names[] = {
		base + "Right" + suffix,
		base + "Left" + suffix,
		base + "Up" + suffix,
		base + "Down" + suffix,
		base + "Back" + suffix,
		base + "Front" + suffix,
	};

	const char* nameStrings[] = {
		names[0].c_str(),
		names[1].c_str(),
		names[2].c_str(),
		names[3].c_str(),
		names[4].c_str(),
		names[5].c_str(),
	};

	auto params = doi::TextureCreateParams{};
	params.format = vk::Format::r8g8b8a8Srgb;
	params.cubemap = true;
	cubemap_ = {wb, readLayers(nameStrings, false), params};
	writeDs();
}

void Skybox::initPipeline(const vpp::Device& dev, vk::RenderPass rp,
		unsigned subpass, vk::SampleCountBits samples) {
	// sampler
	vk::SamplerCreateInfo sci {};
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.minLod = 0.0;
	sci.maxLod = 0.25;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	sampler_ = {dev, sci};

	// ds layout
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex),
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1,
			&sampler_.vkHandle()),
	};

	dsLayout_ = {dev, bindings};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};

	vpp::ShaderModule vertShader(dev, stage_skybox_vert_data);
	vpp::ShaderModule fragShader(dev, stage_skybox_frag_data);
	vpp::GraphicsPipelineInfo gpi {rp, pipeLayout_, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, subpass, samples};

	gpi.depthStencil.depthTestEnable = true;
	gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	// gpi.rasterization.cullMode = vk::CullModeBits::back;
	// gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	// TODO: testing deferred renderer with light scattering
	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.blendEnable = true;
	blendAttachment.colorBlendOp = vk::BlendOp::add;
	blendAttachment.srcColorBlendFactor = vk::BlendFactor::one;
	blendAttachment.dstColorBlendFactor = vk::BlendFactor::one;
	blendAttachment.colorWriteMask =
			vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a;
	gpi.blend.pAttachments = &blendAttachment;

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), NULL, vkpipe);
	pipe_ = {dev, vkpipe};

	// indices
	/*
	indices_ = {dev.bufferAllocator(), 36u * sizeof(std::uint16_t),
		vk::BufferUsageBits::indexBuffer | vk::BufferUsageBits::transferDst,
		dev.deviceMemoryTypes()};
	std::array<std::uint16_t, 36> indices = {
		0, 1, 2,  2, 1, 3, // front
		1, 5, 3,  3, 5, 7, // right
		2, 3, 6,  6, 3, 7, // top
		4, 0, 6,  6, 0, 2, // left
		4, 5, 0,  0, 5, 1, // bottom
		5, 4, 7,  7, 4, 6, // back
	};
	vpp::writeStaging430(indices_, vpp::raw(*indices.data(), 36u));
	*/

	// ubo
	auto uboSize = sizeof(nytl::Mat4f);
	ubo_ = {dev.bufferAllocator(), uboSize, vk::BufferUsageBits::uniformBuffer,
		dev.hostMemoryTypes()};

	// create ds
	ds_ = {ubo_.device().descriptorAllocator(), dsLayout_};
}

void Skybox::writeDs() {
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	dsu.imageSampler({{{}, cubemap_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});

	// NOTE: for debugging the irradiance map
	// dsu.imageSampler({{{}, irradiance_.vkImageView(),
		// vk::ImageLayout::shaderReadOnlyOptimal}});
}

void Skybox::render(vk::CommandBuffer cb) {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pipeLayout_, 0, {{ds_.vkHandle()}}, {});
	// vk::cmdBindIndexBuffer(cb, indices_.buffer(),
	// 	indices_.offset(), vk::IndexType::uint16);
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
}

void Skybox::updateDevice(const nytl::Mat4f& viewProj) {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	doi::write(span, viewProj);
}

} // namespace doi
