#pragma once

#include "pass.hpp"
#include <stage/render.hpp>

// TODO: implement alternative that just uses an additional
// output in light pass

/// Simple pass that calculates light scattering for one light onto an
/// r8Unorm fullscreen target.
/// Needs depth (and based on scattering algorithm also the lights
/// shadow map) as input
class LightScatterPass {
	// TODO
	vpp::RenderPass rp_;
	vpp::Framebuffer fb_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pointPipe_;
	vpp::Pipeline dirPipe_;
	vpp::ViewableImage target_;
};


// dumped
// scatter
if(renderPasses_ & passScattering) {
	vk::cmdBeginRenderPass(cb, {
		scatter_.pass,
		scatter_.fb,
		{0u, 0u, width, height},
		0, nullptr
	}, {});

	vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
	vk::cmdSetViewport(cb, 0, 1, vp);
	vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

	// TODO: scatter support for multiple lights
	vk::DescriptorSet lds;
	if(!dirLights_.empty()) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics,
			scatter_.dirPipe);
		lds = dirLights_[0].ds();
	} else {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics,
			scatter_.pointPipe);
		lds = pointLights_[0].ds();
	}

	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		scatter_.pipeLayout, 0, {{sceneDs_.vkHandle(),
		scatter_.ds.vkHandle(), lds}}, {});
	vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
	vk::cmdEndRenderPass(cb);
}

	// render pass
	auto& dev = device();
	std::array<vk::AttachmentDescription, 1u> attachments;

	// light scattering output format
	attachments[0].format = scatterFormat;
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

	// TODO: just as with ssao, need an additional barrier between
	// this and the consuming pass (in this case light or post processing)
	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	scatter_.pass = {dev, rpi};

	// pipeline
	auto scatterBindings = {
		vpp::descriptorBinding( // depthTex
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &linearSampler_.vkHandle()),
	};

	scatter_.dsLayout = {dev, scatterBindings};
	scatter_.pipeLayout = {dev, {{
		sceneDsLayout_.vkHandle(),
		scatter_.dsLayout.vkHandle(),
		lightDsLayout_.vkHandle()
	}}, {}};

	// TODO: at least for point light, we don't have to use a fullscreen
	// pass here! that really should bring quite the improvement (esp
	// if we later on allow multiple point light scattering effects)
	// TODO: fullscreen shader used by multiple passes, don't reload
	// it every time...
	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule pfragShader(dev, deferred_pointScatter_frag_data);
	vpp::GraphicsPipelineInfo pgpi{scatter_.pass, scatter_.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{pfragShader, vk::ShaderStageBits::fragment},
	}}}};

	pgpi.flags(vk::PipelineCreateBits::allowDerivatives);
	pgpi.depthStencil.depthTestEnable = false;
	pgpi.depthStencil.depthWriteEnable = false;
	pgpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	pgpi.blend.attachmentCount = 1u;
	pgpi.blend.pAttachments = &doi::noBlendAttachment();

	// directionoal
	vpp::ShaderModule dfragShader(dev, deferred_dirScatter_frag_data);
	vpp::GraphicsPipelineInfo dgpi{scatter_.pass, scatter_.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{dfragShader, vk::ShaderStageBits::fragment},
	}}}};

	dgpi.depthStencil = pgpi.depthStencil;
	dgpi.assembly = pgpi.assembly;
	dgpi.blend = pgpi.blend;
	dgpi.base(0);

	vk::Pipeline vkpipes[2];
	auto infos = {pgpi.info(), dgpi.info()};
	vk::createGraphicsPipelines(dev, {}, infos.size(), *infos.begin(),
		nullptr, *vkpipes);

	scatter_.pointPipe = {dev, vkpipes[0]};
	scatter_.dirPipe = {dev, vkpipes[1]};
	scatter_.ds = {dev.descriptorAllocator(), scatter_.dsLayout};
