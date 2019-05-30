#include "postProcess.hpp"
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <nytl/vecOps.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.pp.frag.h>
#include <shaders/deferred.debug.frag.h>

void PostProcessPass::create(InitData& data, const PassCreateInfo& info,
		vk::Format outputFormat) {
	auto& wb = info.wb;
	auto& dev = wb.dev;

	// render pass
	vk::AttachmentDescription attachment;
	attachment.format = outputFormat;
	attachment.samples = vk::SampleCountBits::e1;
	attachment.loadOp = vk::AttachmentLoadOp::dontCare;
	attachment.storeOp = vk::AttachmentStoreOp::store;
	attachment.stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachment.stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachment.initialLayout = vk::ImageLayout::undefined;
	attachment.finalLayout = vk::ImageLayout::presentSrcKHR;

	// subpass
	vk::AttachmentReference colorRef;
	colorRef.attachment = 0u;
	colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = 1u;
	rpi.pAttachments = &attachment;
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	rp_ = {dev, rpi};
	vpp::nameHandle(rp_, "PostProcessPass:rp_");

	// pipe
	auto ppInputBindings = {
		// we use the nearest sampler here since we use it for fxaa and ssr
		// and for ssr we *need* nearest (otherwise we bleed artefacts).
		// Not sure about fxaa but seems to work with nearest.
		// TODO: not sure about fxaa tbh
		vpp::descriptorBinding( // output from combine pass
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.linear),
		// we only need depth for dof. Use nearest sampler for that, we don't
		// interpolate.
		vpp::descriptorBinding( // depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.nearest),
		// params ubo
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
	};

	dsLayout_ = {dev, ppInputBindings};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
	vpp::nameHandle(dsLayout_, "PostProcessPass:dsLayout_");
	vpp::nameHandle(pipeLayout_, "PostProcessPass:pipeLayout_");

	vpp::ShaderModule fragShader(dev, deferred_pp_frag_data);
	vpp::GraphicsPipelineInfo gpi {rp_, pipeLayout_, {{{
		{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}};

	gpi.depthStencil.depthTestEnable = false;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	gpi.blend.attachmentCount = 1u;
	gpi.blend.pAttachments = &doi::noBlendAttachment();
	pipe_ = {dev, gpi.info()};
	vpp::nameHandle(pipe_, "PostProcessPass:pipe_");

	ubo_ = {data.initUbo, wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
	ds_ = {data.initDs, wb.alloc.ds, dsLayout_};

	// debug
	auto debugInputBindings = {
		vpp::descriptorBinding( // albedo
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.nearest),
		vpp::descriptorBinding( // normal
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.nearest),
		vpp::descriptorBinding( // depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.nearest),
		vpp::descriptorBinding( // ssao
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.nearest),
		vpp::descriptorBinding( // ssr
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.nearest),
		vpp::descriptorBinding( // emission
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.nearest),
		vpp::descriptorBinding( // bloom, linear sampling for upsampling
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.linear),
		vpp::descriptorBinding( // luminance
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.nearest),
	};

	debug_.dsLayout = {dev, debugInputBindings};
	vk::PushConstantRange pcr;
	pcr.stageFlags = vk::ShaderStageBits::fragment;
	pcr.size = sizeof(u32);
	debug_.pipeLayout = {dev, {{debug_.dsLayout.vkHandle()}}, {{pcr}}};
	vpp::nameHandle(debug_.dsLayout, "PostProcessPass:debug.dsLayout");
	vpp::nameHandle(debug_.pipeLayout, "PostProcessPass:debug.pipeLayout");

	vpp::ShaderModule debugShader(dev, deferred_debug_frag_data);
	vpp::GraphicsPipelineInfo dgpi{rp_, debug_.pipeLayout, {{{
		{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
		{debugShader, vk::ShaderStageBits::fragment},
	}}}};

	dgpi.depthStencil.depthTestEnable = false;
	dgpi.depthStencil.depthWriteEnable = false;
	dgpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	dgpi.blend.attachmentCount = 1u;
	dgpi.blend.pAttachments = &doi::noBlendAttachment();
	debug_.pipe = {dev, dgpi.info()};
	vpp::nameHandle(debug_.pipe, "PostProcessPass:debug.pipe");

	debug_.ds = {data.initDebugDs, wb.alloc.ds, debug_.dsLayout};
}

void PostProcessPass::init(InitData& data, const PassCreateInfo&) {
	ubo_.init(data.initUbo);
	uboMap_ = ubo_.memoryMap();

	ds_.init(data.initDs);
	vpp::nameHandle(ds_, "PostProcessPass:ds");

	debug_.ds.init(data.initDebugDs);
	vpp::nameHandle(ds_, "PostProcessPass:debug.ds");
}

void PostProcessPass::updateInputs(
		vk::ImageView light, vk::ImageView ldepth,
		vk::ImageView normals,
		vk::ImageView albedo,
		vk::ImageView ssao,
		vk::ImageView ssr,
		vk::ImageView emission,
		vk::ImageView bloom,
		vk::ImageView luminance) {
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler({{{{}, light, vk::ImageLayout::shaderReadOnlyOptimal}}});
	dsu.imageSampler({{{{}, ldepth, vk::ImageLayout::shaderReadOnlyOptimal}}});
	dsu.uniform({{{ubo_}}});
	dsu.apply();

	vpp::DescriptorSetUpdate ddsu(debug_.ds);
	ddsu.imageSampler({{{}, albedo, vk::ImageLayout::shaderReadOnlyOptimal}});
	ddsu.imageSampler({{{}, normals, vk::ImageLayout::shaderReadOnlyOptimal}});
	ddsu.imageSampler({{{}, ldepth, vk::ImageLayout::shaderReadOnlyOptimal}});
	ddsu.imageSampler({{{}, ssao, vk::ImageLayout::shaderReadOnlyOptimal}});
	ddsu.imageSampler({{{}, ssr, vk::ImageLayout::shaderReadOnlyOptimal}});
	ddsu.imageSampler({{{}, emission, vk::ImageLayout::shaderReadOnlyOptimal}});
	ddsu.imageSampler({{{}, bloom, vk::ImageLayout::shaderReadOnlyOptimal}});
	ddsu.imageSampler({{{}, luminance, vk::ImageLayout::shaderReadOnlyOptimal}});
	ddsu.apply();
}

vpp::Framebuffer PostProcessPass::initFramebuffer(vk::ImageView output,
		vk::Extent2D size) {
	auto attachments = {output};
	vk::FramebufferCreateInfo fbi;
	fbi.renderPass = rp_;
	fbi.width = size.width;
	fbi.height = size.height;
	fbi.layers = 1;
	fbi.attachmentCount = attachments.size();
	fbi.pAttachments = attachments.begin();
	return {rp_.device(), fbi};
}

void PostProcessPass::record(vk::CommandBuffer cb, u32 mode) {
	vpp::DebugLabel debugLabel(cb, "PostProcessPass");
	if(mode == modeDefault) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
	} else {
		vk::cmdPushConstants(cb, debug_.pipeLayout,
			vk::ShaderStageBits::fragment, 0, sizeof(mode), &mode);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, debug_.pipe);
		doi::cmdBindGraphicsDescriptors(cb, debug_.pipeLayout, 0, {debug_.ds});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
	}
}

void PostProcessPass::updateDevice() {
	auto span = uboMap_.span();
	doi::write(span, params);
	uboMap_.flush();
}

SyncScope PostProcessPass::dstScopeInput() const {
	return {
		vk::PipelineStageBits::fragmentShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead
	};
}
