#include "scatter.hpp"
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.dirScatter.frag.h>
#include <shaders/deferred.pointScatter.frag.h>

void LightScatterPass::create(InitData& data, const PassCreateInfo& info,
		bool directional) {
	auto& dev = info.wb.dev;
	std::array<vk::AttachmentDescription, 1u> attachments;

	// light scattering output format
	attachments[0].format = format;
	attachments[0].samples = vk::SampleCountBits::e1;
	attachments[0].loadOp = vk::AttachmentLoadOp::dontCare;
	attachments[0].storeOp = vk::AttachmentStoreOp::store;
	attachments[0].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[0].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[0].initialLayout = vk::ImageLayout::undefined;
	attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	// subpass
	vk::AttachmentReference colorRef;
	colorRef.attachment = 0u;
	colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	rp_ = {dev, rpi};
	vpp::nameHandle(rp_, "LightScatterPass:rp");

	// pipeline
	auto scatterBindings = {
		vpp::descriptorBinding( // depthTex
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.linear),
		vpp::descriptorBinding( // ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
	};

	dsLayout_ = {dev, scatterBindings};
	pipeLayout_ = {dev, {{
		info.dsLayouts.camera.vkHandle(),
		dsLayout_.vkHandle(),
		info.dsLayouts.light.vkHandle()
	}}, {}};

	vpp::nameHandle(dsLayout_, "LightScatterPass:dsLayout");
	vpp::nameHandle(pipeLayout_, "LightScatterPass:pipeLayout");

	// TODO: at least for point light, we don't have to use a fullscreen
	// pass here! that really should bring quite the improvement (esp
	// if we later on allow multiple point light scattering effects)
	vpp::ShaderModule fragShader;
	if(directional) {
		fragShader = {dev, deferred_dirScatter_frag_data};
	} else {
		fragShader = {dev, deferred_pointScatter_frag_data};
	}

	vpp::GraphicsPipelineInfo gpi{rp_, pipeLayout_, {{{
		{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}};

	gpi.flags(vk::PipelineCreateBits::allowDerivatives);
	gpi.depthStencil.depthTestEnable = false;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	gpi.blend.attachmentCount = 1u;
	gpi.blend.pAttachments = &doi::noBlendAttachment();
	pipe_ = {dev, gpi.info()};
	vpp::nameHandle(pipe_, "LightScatterPass:pipe");

	ds_ = {data.initDs, info.wb.alloc.ds, dsLayout_};
	ubo_ = {data.initUbo, info.wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
}

void LightScatterPass::init(InitData& data, const PassCreateInfo&) {
	ubo_.init(data.initUbo);
	uboMap_ = ubo_.memoryMap();

	ds_.init(data.initDs);
	vpp::nameHandle(ds_, "LightScatterPass:ds");
}

void LightScatterPass::createBuffers(InitBufferData& data,
		const doi::WorkBatcher& wb, vk::Extent2D size) {
	auto usage = vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::colorAttachment;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, size, usage);
	dlg_assert(vpp::supported(wb.dev, info.img));
	data.viewInfo = info.view;
	target_ = {data.initTarget, wb.alloc.memDevice, info.img,
		wb.dev.deviceMemoryTypes()};
}

void LightScatterPass::initBuffers(InitBufferData& data, vk::Extent2D size,
		vk::ImageView depth) {
	// target
	target_.init(data.initTarget, data.viewInfo);
	vpp::nameHandle(target_, "LightScatterPass:target");

	vk::FramebufferCreateInfo fbi;
	fbi.renderPass = rp_;
	fbi.width = size.width;
	fbi.height = size.height;
	fbi.layers = 1;

	auto attachments = {target_.vkImageView()};
	fbi.attachmentCount = attachments.size();
	fbi.pAttachments = attachments.begin();
	fb_ = {target_.device(), fbi};
	vpp::nameHandle(fb_, "LightScatterPass:fb");

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler({{{}, depth, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.uniform({{{ubo_}}});
	dsu.apply();
}

void LightScatterPass::record(vk::CommandBuffer cb, vk::Extent2D size,
		vk::DescriptorSet scene, vk::DescriptorSet light) {
	vpp::DebugLabel label(cb, "LightScatterPass");
	vk::cmdBeginRenderPass(cb, {
		rp_, fb_,
		{0u, 0u, size.width, size.height},
		0, nullptr
	}, {});

	doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {scene, ds_, light});
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDraw(cb, 4, 1, 0, 0); // tri fan fullscreen
	vk::cmdEndRenderPass(cb);
}

void LightScatterPass::updateDevice() {
	auto span = uboMap_.span();
	doi::write(span, params);
	uboMap_.flush();
}

SyncScope LightScatterPass::srcScopeTarget() const {
	return {
		vk::PipelineStageBits::colorAttachmentOutput,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::colorAttachmentWrite,
	};
}

SyncScope LightScatterPass::dstScopeDepth() const {
	return SyncScope::fragmentSampled();
}
