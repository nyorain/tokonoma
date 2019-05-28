#include "geomLight.hpp"

#include <stage/scene/material.hpp>
#include <stage/scene/primitive.hpp>
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <nytl/vecOps.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.gbuf.vert.h>
#include <shaders/deferred.gbuf.frag.h>
#include <shaders/deferred.pointLight.frag.h>
#include <shaders/deferred.pointLight.vert.h>
#include <shaders/deferred.dirLight.frag.h>

void GeomLightPass::create(InitData& data, const PassCreateInfo& info,
		SyncScope dstNormals,
		SyncScope dstAlbedo,
		SyncScope dstEmission,
		SyncScope dstDepth,
		SyncScope dstLDepth,
		SyncScope dstLight) {
	auto& dev = info.wb.dev;

	// render pass
	// == attachments ==
	std::array<vk::AttachmentDescription, 6> attachments;
	struct {
		unsigned normals = 0;
		unsigned albedo = 1;
		unsigned emission = 2;
		unsigned depth = 3;
		unsigned ldepth = 4;
		unsigned light = 5;
	} ids;

	auto addGBuf = [&](u32 id, vk::Format format, SyncScope scope) {
		attachments[id].format = format;
		attachments[id].samples = vk::SampleCountBits::e1;
		attachments[id].loadOp = vk::AttachmentLoadOp::clear;
		// only store if someone will need it
		attachments[id].storeOp = (scope.layout != vk::ImageLayout::undefined) ?
			vk::AttachmentStoreOp::store : vk::AttachmentStoreOp::dontCare;
		// we clear, so we can discard any old content
		attachments[id].initialLayout = vk::ImageLayout::undefined;
		attachments[id].finalLayout = (scope.layout == vk::ImageLayout::undefined) ?
			vk::ImageLayout::shaderReadOnlyOptimal : scope.layout;
		// we never use stencil anywhere
		attachments[id].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[id].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	};

	addGBuf(ids.normals, normalsFormat, dstNormals);
	addGBuf(ids.albedo, albedoFormat, dstAlbedo);
	addGBuf(ids.emission, emissionFormat, dstEmission);
	addGBuf(ids.depth, info.depthFormat, dstDepth);
	addGBuf(ids.ldepth, ldepthFormat, dstLDepth);
	addGBuf(ids.light, lightFormat, dstLight);

	// == subpasses ==
	vk::AttachmentReference gbufRefs[4];
	gbufRefs[0].attachment = ids.normals;
	gbufRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[1].attachment = ids.albedo;
	gbufRefs[1].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[2].attachment = ids.emission;
	gbufRefs[2].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[3].attachment = ids.ldepth;
	gbufRefs[3].layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference ginputRefs[3];
	ginputRefs[0].attachment = ids.normals;
	ginputRefs[0].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	ginputRefs[1].attachment = ids.albedo;
	ginputRefs[1].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	ginputRefs[2].attachment = ids.ldepth;
	ginputRefs[2].layout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::AttachmentReference depthRef;
	depthRef.attachment = ids.depth;
	depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	vk::AttachmentReference lightRef;
	lightRef.attachment = ids.light;
	lightRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	std::array<vk::SubpassDescription, 2u> subpasses;
	auto& gpass = subpasses[0];
	auto& lpass = subpasses[1];

	gpass.colorAttachmentCount = 4;
	gpass.pColorAttachments = gbufRefs;
	gpass.pDepthStencilAttachment = &depthRef;

	lpass.colorAttachmentCount = 1;
	lpass.pColorAttachments = &lightRef;
	// use depth stencil attachment for light boxes
	lpass.pDepthStencilAttachment = &depthRef;
	lpass.inputAttachmentCount = 3;
	lpass.pInputAttachments = ginputRefs;

	// == dependencies ==
	std::array<vk::SubpassDependency, 3> deps;

	// deps[0]: make sure gbuffers can be read by light pass
	// setting this flag is extremely important to allow tiled
	// optimizations
	deps[0].dependencyFlags = vk::DependencyBits::byRegion;
	deps[0].srcSubpass = 0u;
	deps[0].srcStageMask =
		vk::PipelineStageBits::colorAttachmentOutput |
		vk::PipelineStageBits::lateFragmentTests;
	deps[0].srcAccessMask =
		vk::AccessBits::colorAttachmentWrite |
		vk::AccessBits::depthStencilAttachmentWrite;
	deps[0].dstSubpass = 1u;
	deps[0].dstStageMask = vk::PipelineStageBits::fragmentShader |
		vk::PipelineStageBits::earlyFragmentTests;
	deps[0].dstAccessMask = vk::AccessBits::inputAttachmentRead |
		vk::AccessBits::depthStencilAttachmentRead;

	// deps[1]: make sure gbuffers can be accessed afterwards
	// (non-linear) depth buffer not relevant here
	deps[1].srcSubpass = 0u;
	deps[1].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	deps[1].srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	deps[1].dstSubpass = vk::subpassExternal;
	deps[1].dstStageMask = dstNormals.stages |
		dstAlbedo.stages |
		dstEmission.stages |
		dstLDepth.stages;
	deps[1].dstAccessMask = dstNormals.access |
		dstAlbedo.access |
		dstEmission.access |
		dstLDepth.access;

	// deps[2]: make sure light buffer can be accessed afterwrads
	deps[2].srcSubpass = 0u;
	deps[2].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	deps[2].srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	deps[2].dstSubpass = vk::subpassExternal;
	deps[2].dstStageMask = dstLight.stages;
	deps[2].dstAccessMask = dstLight.access;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.dependencyCount = deps.size();
	rpi.pDependencies = deps.data();
	rpi.subpassCount = subpasses.size();
	rpi.pSubpasses = subpasses.data();

	rp_ = {dev, rpi};
	vpp::nameHandle(rp_, "GeomLightPass:rp_");

	// pipeline
	geomPipeLayout_ = {dev, {{
		info.dsLayouts.scene.vkHandle(),
		info.dsLayouts.material.vkHandle(),
		info.dsLayouts.primitive.vkHandle(),
	}}, {{doi::Material::pcr()}}};
	vpp::nameHandle(geomPipeLayout_, "GeomLightPass:geomPipeLayout_");

	vpp::ShaderModule vertShader(dev, deferred_gbuf_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_gbuf_frag_data);
	vpp::GraphicsPipelineInfo gpi {rp_, geomPipeLayout_, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, 0};

	gpi.vertex = doi::Primitive::vertexInfo();

	// we don't blend in the gbuffers; simply overwrite
	auto blendAttachments = {
		doi::noBlendAttachment(),
		doi::noBlendAttachment(),
		doi::noBlendAttachment(),
		doi::noBlendAttachment(),
	};

	gpi.blend.attachmentCount = blendAttachments.size();
	gpi.blend.pAttachments = blendAttachments.begin();
	gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

	gpi.depthStencil.depthTestEnable = true;
	gpi.depthStencil.depthWriteEnable = true;
	gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

	// NOTE: see the gltf material.doubleSided property. We can't switch
	// this per material (without requiring two pipelines) so we simply
	// always render backfaces currently and then dynamically cull in the
	// fragment shader. That is required since some models rely on
	// backface culling for effects (e.g. outlines). See model.frag
	gpi.rasterization.cullMode = vk::CullModeBits::none;
	gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
	geomPipe_ = {dev, gpi.info()};
	vpp::nameHandle(geomPipe_, "GeomLightPass:geomPipe_");


	// light
	auto inputBindings = {
		vpp::descriptorBinding( // normal
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // albedo
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // depth
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
	};

	lightDsLayout_ = {dev, inputBindings};

	// light ds
	lightPipeLayout_ = {dev, {{
		info.dsLayouts.scene.vkHandle(),
		lightDsLayout_.vkHandle(),
		info.dsLayouts.light.vkHandle(),
	}}, {}};
	vpp::nameHandle(lightDsLayout_, "GeomLightPass:lightDsLayout_");
	vpp::nameHandle(lightPipeLayout_, "GeomLightPass:lightPipeLayout_");

	vpp::ShaderModule pointVertShader(dev, deferred_pointLight_vert_data);
	vpp::ShaderModule pointFragShader(dev, deferred_pointLight_frag_data);
	vpp::GraphicsPipelineInfo pgpi{rp_, lightPipeLayout_, {{{
		{pointVertShader, vk::ShaderStageBits::vertex},
		{pointFragShader, vk::ShaderStageBits::fragment},
	}}}, 1};

	// TODO: enable depth test for additional discarding by rasterizer
	// (better performance i guess). requires depth attachment in this pass
	// though. Don't enable depth write!
	pgpi.depthStencil.depthTestEnable = false;
	pgpi.depthStencil.depthWriteEnable = false;
	pgpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	// TODO: hack
	// have to figure out how correctly draw/switch inside/outside box
	// in vertex shader...
	pgpi.rasterization.cullMode = vk::CullModeBits::front;
	pgpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	// additive blending
	vk::PipelineColorBlendAttachmentState lightBlends[1];
	lightBlends[0].blendEnable = true;
	lightBlends[0].colorBlendOp = vk::BlendOp::add;
	lightBlends[0].srcColorBlendFactor = vk::BlendFactor::one;
	lightBlends[0].dstColorBlendFactor = vk::BlendFactor::one;
	lightBlends[0].alphaBlendOp = vk::BlendOp::add;
	lightBlends[0].srcAlphaBlendFactor = vk::BlendFactor::one;
	lightBlends[0].dstAlphaBlendFactor = vk::BlendFactor::one;
	lightBlends[0].colorWriteMask =
		vk::ColorComponentBits::r |
		vk::ColorComponentBits::g |
		vk::ColorComponentBits::b |
		vk::ColorComponentBits::a;

	pgpi.blend.attachmentCount = 1u;
	pgpi.blend.pAttachments = lightBlends;
	pgpi.flags(vk::PipelineCreateBits::allowDerivatives);

	// dir light
	// here we can use a fullscreen shader pass since directional lights
	// don't have a light volume, they fill the whole screen
	vpp::ShaderModule dirFragShader(dev, deferred_dirLight_frag_data);
	vpp::GraphicsPipelineInfo lgpi{rp_, lightPipeLayout_, {{{
		{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
		{dirFragShader, vk::ShaderStageBits::fragment},
	}}}, 1};

	lgpi.base(0); // base index
	lgpi.blend = pgpi.blend;
	lgpi.depthStencil = pgpi.depthStencil;
	lgpi.assembly = pgpi.assembly;
	lgpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

	// create the pipes
	vk::GraphicsPipelineCreateInfo infos[] = {pgpi.info(), lgpi.info()};
	vk::Pipeline vkpipes[2];
	vk::createGraphicsPipelines(dev, {}, 2, *infos, nullptr, *vkpipes);
	pointLightPipe_ = {dev, vkpipes[0]};
	dirLightPipe_ = {dev, vkpipes[1]};
	vpp::nameHandle(pointLightPipe_, "GeomLightPass:pointLightPipe_");
	vpp::nameHandle(dirLightPipe_, "GeomLightPass:dirLightPipe_");

	lightDs_ = {data.initLightDs, info.wb.alloc.ds, lightDsLayout_};
}

void GeomLightPass::init(InitData& data) {
	lightDs_.init(data.initLightDs);
	vpp::nameHandle(lightDs_, "GeomLightPass:lightDs");
}

void GeomLightPass::createBuffers(InitBufferData& data,
		const doi::WorkBatcher& wb, vk::Extent2D size) {
	auto createInfo = [&](vk::Format format,
			vk::ImageUsageFlags usage) {
		auto info = vpp::ViewableImageCreateInfo(format,
			vk::ImageAspectBits::color, {size.width, size.height}, usage);
		dlg_assert(vpp::supported(wb.dev, info.img));
		return info;
	};

	constexpr auto baseUsage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::inputAttachment |
		vk::ImageUsageBits::sampled;
	constexpr auto ldepthUsage = baseUsage | vk::ImageUsageBits::transferSrc;
	constexpr auto emissionUsage = baseUsage |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::storage;
	constexpr auto lightUsage = baseUsage | vk::ImageUsageBits::storage;

	auto info = createInfo(normalsFormat, baseUsage);
	normals_ = {data.initNormals.initTarget, wb.alloc.memDevice, info.img};
	data.initNormals.viewInfo = info.view;

	info = createInfo(albedoFormat, baseUsage);
	albedo_ = {data.initAlbedo.initTarget, wb.alloc.memDevice, info.img};
	data.initAlbedo.viewInfo = info.view;

	info = createInfo(ldepthFormat, ldepthUsage);
	ldepth_ = {data.initDepth.initTarget, wb.alloc.memDevice, info.img};
	data.initDepth.viewInfo = info.view;

	info = createInfo(emissionFormat, emissionUsage);
	emission_ = {data.initEmission.initTarget, wb.alloc.memDevice, info.img};
	data.initEmission.viewInfo = info.view;

	info = createInfo(lightFormat, lightUsage);
	light_ = {data.initLight.initTarget, wb.alloc.memDevice, info.img};
	data.initLight.viewInfo = info.view;
}

void GeomLightPass::initBuffers(InitBufferData& data, vk::Extent2D size,
		vk::ImageView depth) {
	normals_.init(data.initNormals.initTarget, data.initNormals.viewInfo);
	albedo_.init(data.initAlbedo.initTarget, data.initAlbedo.viewInfo);
	ldepth_.init(data.initDepth.initTarget, data.initDepth.viewInfo);
	emission_.init(data.initEmission.initTarget, data.initEmission.viewInfo);
	light_.init(data.initLight.initTarget, data.initLight.viewInfo);

	vpp::nameHandle(normals_, "GeomLightPass:normals_");
	vpp::nameHandle(albedo_, "GeomLightPass:albedo_");
	vpp::nameHandle(ldepth_, "GeomLightPass:ldepth_");
	vpp::nameHandle(emission_, "GeomLightPass:emission_");
	vpp::nameHandle(light_, "GeomLightPass:light_");

	// framebuffer
	auto attachments = {
		normals_.vkImageView(),
		albedo_.vkImageView(),
		emission_.vkImageView(),
		depth,
		ldepth_.vkImageView(),
		light_.vkImageView(),
	};

	vk::FramebufferCreateInfo fbi;
	fbi.renderPass = rp_;
	fbi.width = size.width;
	fbi.height = size.height;
	fbi.layers = 1;
	fbi.attachmentCount = attachments.size();
	fbi.pAttachments = attachments.begin();
	fb_ = {rp_.device(), fbi};

	// ds
	vpp::DescriptorSetUpdate dsu(lightDs_);
	dsu.inputAttachment({{{}, normals_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.inputAttachment({{{}, albedo_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.inputAttachment({{{}, ldepth_.imageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
}

void GeomLightPass::record(vk::CommandBuffer cb, const vk::Extent2D& size,
		vk::DescriptorSet sceneDs, const doi::Scene& scene,
		nytl::Span<doi::PointLight> pointLights,
		nytl::Span<doi::DirLight> dirLights, vpp::BufferSpan boxIndices) {
	auto width = size.width;
	auto height = size.height;

	std::array<vk::ClearValue, 6u> cv {};
	cv[0] = {-1.f, -1.f, -1.f, -1.f}; // normals, rgba16f snorm
	cv[1] = {0, 0, 0, 0}; // albedo, rgba8
	cv[2] = {{0.f, 0.f, 0.f, 0.f}}; // emission, rgba16f
	cv[3].depthStencil = {1.f, 0u}; // depth
	cv[4] = {{1000.f, 0.f, 0.f, 0.f}}; // linear r16f depth
	cv[5] = {{0.f, 0.f, 0.f, 0.f}}; // light, rgba16f
	vk::cmdBeginRenderPass(cb, {rp_, fb_,
		{0u, 0u, width, height},
		std::uint32_t(cv.size()), cv.data()
	}, {});

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, geomPipe_);
	doi::cmdBindGraphicsDescriptors(cb, geomPipeLayout_, 0, {sceneDs});
	scene.render(cb, geomPipeLayout_);

	// render light balls with emission material
	// NOTE: rendering them messes with light scattering since
	// then the light source is *inside* something (small).
	// disabled for now, although it shows bloom nicely.
	// Probably best to render it in the last pass, together
	// with the skybox. Should use the depth buffer correctly though!
	//
	// lightMaterial_->bind(cb, gpass_.pipeLayout);
	// for(auto& l : pointLights_) {
	// 	l.lightBall().render(cb, gpass_.pipeLayout);
	// }
	// for(auto& l : dirLights_) {
	// 	l.lightBall().render(cb, gpass_.pipeLayout);
	// }

	vk::cmdNextSubpass(cb, vk::SubpassContents::eInline);

	doi::cmdBindGraphicsDescriptors(cb, lightPipeLayout_, 0, {sceneDs, lightDs_});
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pointLightPipe_);
	vk::cmdBindIndexBuffer(cb, boxIndices.buffer(),
		boxIndices.offset(), vk::IndexType::uint16);
	for(auto& light : pointLights) {
		doi::cmdBindGraphicsDescriptors(cb, lightPipeLayout_, 2, {light.ds()});
		vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0); // box
	}

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, dirLightPipe_);
	for(auto& light : dirLights) {
		doi::cmdBindGraphicsDescriptors(cb, lightPipeLayout_, 2, {light.ds()});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad
	}

	vk::cmdEndRenderPass(cb);
}

// SyncScope GeomLightPass::srcScopeLight() const {
// 	return {
// 		vk::PipelineStageBits::colorAttachmentOutput,
// 		vk::ImageLayout::shaderReadOnlyOptimal,
// 		vk::AccessBits::colorAttachmentRead |
// 			vk::AccessBits::colorAttachmentWrite;
// 	};
// }

