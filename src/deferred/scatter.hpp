#pragma once

#include "pass.hpp"
#include <stage/render.hpp>

// TODO: implement alternative that just uses an additional
// output in light pass
// TODO: could optionally make this a compute pass (fullscreen)

/// Simple pass that calculates light scattering for one light onto an
/// r8Unorm fullscreen target.
/// Needs depth (and based on scattering algorithm also the lights
/// shadow map) as input
class LightScatterPass {
public:
	static constexpr vk::Format format = vk::Format::r8Unorm;
	static constexpr u32 flagShadow = 1 << 0u;
	static constexpr u32 flagAttenuation = 1 << 1u; // only point lights

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
	};

public:
	// See scatter.glsl, point/dirScatter.frag
	struct {
		u32 flags {flagShadow | flagAttenuation};
		float fac {1.f};
		float mie {0.1f};
	} params;

	// Needs recreation after being changed, specialization constant
	// to allow driver unrolling at pipeline compliation time
	u32 sampleCount = 10u;

public:
	LightScatterPass() = default;
	void create(InitData&, const PassCreateInfo&,
		bool directional, SyncScope dstTarget);
	void init(InitData&, const PassCreateInfo&);

	void createBuffers(InitBufferData&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::Extent2D,
		vk::ImageView depth);

	void record(vk::CommandBuffer, vk::Extent2D,
		vk::DescriptorSet scene, vk::DescriptorSet light);
	void updateDevice();

	SyncScope dstScopeDepth() const;

protected:
	vpp::RenderPass rp_;
	vpp::Framebuffer fb_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	// vpp::Pipeline pointPipe_;
	// vpp::Pipeline dirPipe_;
	vpp::ViewableImage target_;

	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
};


/*
// dumped
// scatter
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
*/
