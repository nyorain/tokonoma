#include "geomLight.hpp"

#include <stage/scene/environment.hpp>
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
#include <shaders/deferred.ao.frag.h>
#include <shaders/deferred.blend.frag.h>
#include <shaders/deferred.transparent.frag.h>

// NOTE: current order independent transparency mainly from
// - http://casual-effects.blogspot.com/2014/03/weighted-blended-order-independent.html
// - (http://casual-effects.blogspot.com/2015/03/implemented-weighted-blended-order.html)
// - (http://casual-effects.blogspot.com/2015/03/colored-blended-order-independent.html)

void GeomLightPass::create(InitData& data, const PassCreateInfo& info,
		SyncScope dstNormals,
		SyncScope dstAlbedo,
		SyncScope dstEmission,
		SyncScope dstDepth,
		SyncScope dstLDepth,
		SyncScope dstLight, bool ao) {
	auto& dev = info.wb.dev;

	// render pass
	// == attachments ==
	std::array<vk::AttachmentDescription, 8> attachments;
	struct {
		unsigned normals = 0;
		unsigned albedo = 1;
		unsigned emission = 2;
		unsigned depth = 3;
		unsigned ldepth = 4;
		unsigned light = 5;
		unsigned refl = 6;
		unsigned revealage = 7;
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
	// we don't need those two anymore after the render pass
	addGBuf(ids.refl, reflFormat, {});
	addGBuf(ids.revealage, revealageFormat, {});

	// == subpasses ==
	std::array<vk::SubpassDescription, 4u> subpasses;
	auto& gpass = subpasses[0]; // geometry into gbuffers
	auto& lpass = subpasses[1]; // light, reading gbuffers, shading,
	auto& tpass = subpasses[2]; // transparent geometry, refl&revealage
	auto& bpass = subpasses[3]; // blending transparent and deferred

	// gpass
	vk::AttachmentReference gbufRefs[4];
	gbufRefs[0].attachment = ids.normals;
	gbufRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[1].attachment = ids.albedo;
	gbufRefs[1].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[2].attachment = ids.emission;
	gbufRefs[2].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[3].attachment = ids.ldepth;
	gbufRefs[3].layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference depthRef;
	depthRef.attachment = ids.depth;
	depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	gpass.colorAttachmentCount = 4;
	gpass.pColorAttachments = gbufRefs;
	gpass.pDepthStencilAttachment = &depthRef;

	// lpass
	vk::AttachmentReference ginputRefs[4];
	ginputRefs[0].attachment = ids.normals;
	ginputRefs[0].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	ginputRefs[1].attachment = ids.albedo;
	ginputRefs[1].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	ginputRefs[2].attachment = ids.ldepth;
	ginputRefs[2].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	ginputRefs[3].attachment = ids.emission;
	ginputRefs[3].layout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::AttachmentReference lightRef;
	lightRef.attachment = ids.light;
	lightRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	lpass.colorAttachmentCount = 1;
	lpass.pColorAttachments = &lightRef;
	// use depth stencil attachment for light boxes
	lpass.pDepthStencilAttachment = &depthRef;
	lpass.inputAttachmentCount = 3 + ao;
	lpass.pInputAttachments = ginputRefs;

	// tpass
	vk::AttachmentReference tRenderRefs[2];
	tRenderRefs[0].attachment = ids.refl;
	tRenderRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;
	tRenderRefs[1].attachment = ids.revealage;
	tRenderRefs[1].layout = vk::ImageLayout::colorAttachmentOptimal;

	tpass.colorAttachmentCount = 2;
	tpass.pColorAttachments = tRenderRefs;
	tpass.pDepthStencilAttachment = &depthRef; // readonly

	// bpass
	vk::AttachmentReference bInputRefs[2];
	bInputRefs[0].attachment = ids.refl;
	bInputRefs[0].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	bInputRefs[1].attachment = ids.revealage;
	bInputRefs[1].layout = vk::ImageLayout::shaderReadOnlyOptimal;

	bpass.colorAttachmentCount = 1;
	bpass.pColorAttachments = &lightRef;
	bpass.inputAttachmentCount = 2u;
	bpass.pInputAttachments = bInputRefs;

	// TODO: we probably needd a dependency from 0 to 2 that makes
	// sure depth can be read in that pass, right?
	// == dependencies ==
	std::array<vk::SubpassDependency, 5> deps;

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
	deps[1].dstStageMask = vk::PipelineStageBits::bottomOfPipe |
		dstNormals.stages |
		dstAlbedo.stages |
		dstEmission.stages |
		dstLDepth.stages;
	deps[1].dstAccessMask = dstNormals.access |
		dstAlbedo.access |
		dstEmission.access |
		dstLDepth.access;

	// deps[2]: make sure light buffer from light pass (subpass 1) can
	// be accessed in combine pass (subpass 3)
	deps[2].dependencyFlags = vk::DependencyBits::byRegion;
	deps[2].srcSubpass = 1u;
	deps[2].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	deps[2].srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	deps[2].dstSubpass = 3u;
	deps[2].dstStageMask = vk::PipelineStageBits::fragmentShader;
	deps[2].dstAccessMask = vk::AccessBits::shaderRead;

	// deps[3]: make sure order independent transparency buffers can
	// be accessed in combine pass
	deps[3].dependencyFlags = vk::DependencyBits::byRegion;
	deps[3].srcSubpass = 2u;
	deps[3].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	deps[3].srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	deps[3].dstSubpass = 3u;
	deps[3].dstStageMask = vk::PipelineStageBits::fragmentShader;
	deps[3].dstAccessMask = vk::AccessBits::shaderRead;

	// deps[4]: make sure light buffer can be accessed afterwrads
	deps[4].srcSubpass = 3u;
	deps[4].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	deps[4].srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	deps[4].dstSubpass = vk::subpassExternal;
	deps[4].dstStageMask = vk::PipelineStageBits::bottomOfPipe |
		dstLight.stages;
	deps[4].dstAccessMask = dstLight.access;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.dependencyCount = deps.size();
	rpi.pDependencies = deps.data();
	rpi.subpassCount = subpasses.size();
	rpi.pSubpasses = subpasses.data();

	rp_ = {dev, rpi};
	vpp::nameHandle(rp_, "GeomLightPass:rp");

	// pipeline
	geomPipeLayout_ = {dev, {{
		info.dsLayouts.scene.vkHandle(),
		info.dsLayouts.material.vkHandle(),
		info.dsLayouts.primitive.vkHandle(),
	}}, {{doi::Material::pcr()}}};
	vpp::nameHandle(geomPipeLayout_, "GeomLightPass:geomPipeLayout");

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
	vpp::nameHandle(geomPipe_, "GeomLightPass:geomPipe");


	// light
	auto lightBindings = {
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

	lightDsLayout_ = {dev, lightBindings};
	lightPipeLayout_ = {dev, {{
		info.dsLayouts.scene.vkHandle(),
		lightDsLayout_.vkHandle(),
		info.dsLayouts.light.vkHandle(),
	}}, {}};
	vpp::nameHandle(lightDsLayout_, "GeomLightPass:lightDsLayout");
	vpp::nameHandle(lightPipeLayout_, "GeomLightPass:lightPipeLayout");

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
	vpp::nameHandle(pointLightPipe_, "GeomLightPass:pointLightPipe");
	vpp::nameHandle(dirLightPipe_, "GeomLightPass:dirLightPipe");

	lightDs_ = {data.initLightDs, info.wb.alloc.ds, lightDsLayout_};


	// transparent geom rendering pipeline
	vpp::ShaderModule transparentFragShader(dev, deferred_transparent_frag_data);
	vpp::GraphicsPipelineInfo tgpi {rp_, geomPipeLayout_, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{transparentFragShader, vk::ShaderStageBits::fragment},
	}}}, 2};

	tgpi.vertex = doi::Primitive::vertexInfo();
	tgpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	tgpi.depthStencil.depthTestEnable = true;
	tgpi.depthStencil.depthWriteEnable = false;
	tgpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

	// NOTE: we require the independent blend device feature here
	vk::PipelineColorBlendAttachmentState bas[2];
	bas[0].blendEnable = true;
	bas[0].colorBlendOp = vk::BlendOp::add;
	bas[0].srcColorBlendFactor = vk::BlendFactor::one;
	bas[0].dstColorBlendFactor = vk::BlendFactor::one;
	bas[0].srcAlphaBlendFactor = vk::BlendFactor::one;
	bas[0].dstAlphaBlendFactor = vk::BlendFactor::one;
	bas[0].colorWriteMask =
		vk::ColorComponentBits::r |
		vk::ColorComponentBits::g |
		vk::ColorComponentBits::b |
		vk::ColorComponentBits::a;

	bas[1] = bas[0];
	bas[1].srcColorBlendFactor = vk::BlendFactor::zero;
	bas[1].dstColorBlendFactor = vk::BlendFactor::oneMinusSrcColor;

	tgpi.blend.attachmentCount = 2;
	tgpi.blend.pAttachments = bas;

	transparentPipe_ = {dev, tgpi.info()};
	vpp::nameHandle(transparentPipe_, "GeomLightPass:transparentPipe");


	// blend/combine pipe
	auto blendBindings = {
		vpp::descriptorBinding( // refl
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // reveal
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
	};

	blendDsLayout_ = {dev, blendBindings};
	blendPipeLayout_ = {dev, {{ blendDsLayout_.vkHandle(), }}, {}};
	vpp::nameHandle(blendDsLayout_, "GeomLightPass:blendDsLayout");
	vpp::nameHandle(blendPipeLayout_, "GeomLightPass:blendPipeLayout");

	vpp::ShaderModule blendFragShader(dev, deferred_blend_frag_data);
	vpp::GraphicsPipelineInfo bgpi {rp_, blendPipeLayout_, {{{
		{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
		{blendFragShader, vk::ShaderStageBits::fragment},
	}}}, 3};

	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.blendEnable = true;
	blendAttachment.colorBlendOp = vk::BlendOp::add;
	blendAttachment.srcColorBlendFactor = vk::BlendFactor::oneMinusSrcAlpha;
	blendAttachment.dstColorBlendFactor = vk::BlendFactor::srcAlpha;
	blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::zero;
	blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::one;
	blendAttachment.colorWriteMask =
		vk::ColorComponentBits::r |
		vk::ColorComponentBits::g |
		vk::ColorComponentBits::b |
		vk::ColorComponentBits::a;

	bgpi.blend.attachmentCount = 1;
	bgpi.blend.pAttachments = &blendAttachment;

	blendPipe_ = {dev, bgpi.info()};
	vpp::nameHandle(blendPipe_, "GeomLightPass:blendPipe");

	blendDs_ = {data.initBlendDs, info.wb.alloc.ds, blendDsLayout_};


	// init ao, if done here
	if(!ao) {
		aoPipe_ = {};
		return;
	}

	auto aoBindings = {
		vpp::descriptorBinding( // normal
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // albedo
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // depth
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // emission
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // irradiance
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.linear),
		vpp::descriptorBinding( // envMap
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.linear),
		vpp::descriptorBinding( // brdflut
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &info.samplers.linear),
		vpp::descriptorBinding( // ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
	};

	aoDsLayout_ = {dev, aoBindings};
	vk::PushConstantRange pcr;
	pcr.size = 4u;
	pcr.stageFlags = vk::ShaderStageBits::fragment;
	aoPipeLayout_ = {dev, {{
		info.dsLayouts.scene.vkHandle(),
		aoDsLayout_.vkHandle(),
	}}, {{pcr}}};
	vpp::nameHandle(aoDsLayout_, "GeomLightPass:aoDsLayout");
	vpp::nameHandle(aoPipeLayout_, "GeomLightPass:aoPipeLayout");

	vpp::ShaderModule aoFragShader(dev, deferred_ao_frag_data);
	vpp::GraphicsPipelineInfo aogpi{rp_, aoPipeLayout_, {{{
		{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
		{aoFragShader, vk::ShaderStageBits::fragment},
	}}}, 1};

	aogpi.depthStencil.depthTestEnable = false;
	aogpi.depthStencil.depthWriteEnable = false;

	// additive blending just like the light pipes
	aogpi.blend.attachmentCount = 1u;
	aogpi.blend.pAttachments = lightBlends;
	aoPipe_ = {dev, aogpi.info()};
	vpp::nameHandle(aoPipe_, "GeomLightPass:aoPipe");

	aoUbo_ = {data.initAoUbo, info.wb.alloc.bufHost, sizeof(aoParams),
		vk::BufferUsageBits::uniformBuffer, info.wb.dev.hostMemoryTypes()};
	aoDs_ = {data.initAoDs, info.wb.alloc.ds, aoDsLayout_};
}

void GeomLightPass::init(InitData& data) {
	lightDs_.init(data.initLightDs);
	vpp::nameHandle(lightDs_, "GeomLightPass:lightDs");

	blendDs_.init(data.initBlendDs);
	vpp::nameHandle(blendDs_, "GeomLightPass:blendDs");

	if(renderAO()) {
		aoUbo_.init(data.initAoUbo);
		aoUboMap_ = aoUbo_.memoryMap();

		aoDs_.init(data.initAoDs);
		vpp::nameHandle(aoDs_, "GeomLightPass:aoDs");
	}
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

	auto devMem = wb.dev.deviceMemoryTypes();
	constexpr auto baseUsage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::inputAttachment |
		vk::ImageUsageBits::sampled;
	constexpr auto ldepthUsage = baseUsage | vk::ImageUsageBits::transferSrc;
	constexpr auto emissionUsage = baseUsage |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::storage;
	constexpr auto lightUsage = baseUsage | vk::ImageUsageBits::storage;
	constexpr auto transparencyUsage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::inputAttachment;

	auto info = createInfo(normalsFormat, baseUsage);
	normals_ = {data.initNormals.initTarget, wb.alloc.memDevice, info.img,
		devMem};
	data.initNormals.viewInfo = info.view;

	info = createInfo(albedoFormat, baseUsage);
	albedo_ = {data.initAlbedo.initTarget, wb.alloc.memDevice, info.img,
		devMem};
	data.initAlbedo.viewInfo = info.view;

	info = createInfo(ldepthFormat, ldepthUsage);
	ldepth_ = {data.initDepth.initTarget, wb.alloc.memDevice, info.img,
		devMem};
	data.initDepth.viewInfo = info.view;

	info = createInfo(emissionFormat, emissionUsage);
	emission_ = {data.initEmission.initTarget, wb.alloc.memDevice, info.img,
		devMem};
	data.initEmission.viewInfo = info.view;

	info = createInfo(lightFormat, lightUsage);
	light_ = {data.initLight.initTarget, wb.alloc.memDevice, info.img,
		devMem};
	data.initLight.viewInfo = info.view;

	info = createInfo(reflFormat, transparencyUsage);
	reflTarget_ = {data.initRefl.initTarget, wb.alloc.memDevice, info.img,
		devMem};
	data.initRefl.viewInfo = info.view;

	info = createInfo(revealageFormat, transparencyUsage);
	revealageTarget_ = {data.initRevealage.initTarget, wb.alloc.memDevice,
		info.img, devMem};
	data.initRevealage.viewInfo = info.view;
}

void GeomLightPass::initBuffers(InitBufferData& data, vk::Extent2D size,
		vk::ImageView depth, vk::ImageView irradiance, vk::ImageView envMap,
		unsigned envLods, vk::ImageView brdflut) {
	normals_.init(data.initNormals.initTarget, data.initNormals.viewInfo);
	albedo_.init(data.initAlbedo.initTarget, data.initAlbedo.viewInfo);
	ldepth_.init(data.initDepth.initTarget, data.initDepth.viewInfo);
	emission_.init(data.initEmission.initTarget, data.initEmission.viewInfo);
	light_.init(data.initLight.initTarget, data.initLight.viewInfo);

	reflTarget_.init(data.initRefl.initTarget, data.initRefl.viewInfo);
	revealageTarget_.init(data.initRevealage.initTarget,
		data.initRevealage.viewInfo);

	vpp::nameHandle(normals_, "GeomLightPass:normals");
	vpp::nameHandle(albedo_, "GeomLightPass:albedo");
	vpp::nameHandle(ldepth_, "GeomLightPass:ldepth");
	vpp::nameHandle(emission_, "GeomLightPass:emission");
	vpp::nameHandle(light_, "GeomLightPass:light");
	vpp::nameHandle(reflTarget_, "GeomLightPass:reflTarget");
	vpp::nameHandle(revealageTarget_, "GeomLightPass:revealageTarget");

	// framebuffer
	auto attachments = {
		normals_.vkImageView(),
		albedo_.vkImageView(),
		emission_.vkImageView(),
		depth,
		ldepth_.vkImageView(),
		light_.vkImageView(),
		reflTarget_.vkImageView(),
		revealageTarget_.vkImageView(),
	};

	vk::FramebufferCreateInfo fbi;
	fbi.renderPass = rp_;
	fbi.width = size.width;
	fbi.height = size.height;
	fbi.layers = 1;
	fbi.attachmentCount = attachments.size();
	fbi.pAttachments = attachments.begin();
	fb_ = {rp_.device(), fbi};
	vpp::nameHandle(fb_, "GeomLightPass:fb");

	// ds
	vpp::DescriptorSetUpdate ldsu(lightDs_);
	ldsu.inputAttachment({{{}, normals_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.inputAttachment({{{}, albedo_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.inputAttachment({{{}, ldepth_.imageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.apply();

	vpp::DescriptorSetUpdate bdsu(blendDs_);
	bdsu.inputAttachment({{{}, reflTarget_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	bdsu.inputAttachment({{{}, revealageTarget_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	bdsu.apply();

	if(renderAO()) {
		vpp::DescriptorSetUpdate dsu(aoDs_);
		dsu.inputAttachment({{{}, normals_.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.inputAttachment({{{}, albedo_.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.inputAttachment({{{}, ldepth_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.inputAttachment({{{}, emission_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, irradiance, vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, envMap, vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, brdflut, vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.uniform({{{aoUbo_}}});

		aoEnvLods_ = envLods;
	}
}

void GeomLightPass::record(vk::CommandBuffer cb, const vk::Extent2D& size,
		vk::DescriptorSet sceneDs, const doi::Scene& scene,
		nytl::Span<doi::PointLight> pointLights,
		nytl::Span<doi::DirLight> dirLights, vpp::BufferSpan boxIndices,
		const doi::Environment* env, TimeWidget& time) {
	vpp::DebugLabel(cb, "GeomLightPass");
	auto width = size.width;
	auto height = size.height;

	std::array<vk::ClearValue, 8u> cv {};
	cv[0] = {-1.f, -1.f, -1.f, -1.f}; // normals, rgba16f snorm
	cv[1] = {0, 0, 0, 0}; // albedo, rgba8
	cv[2] = {{0.f, 0.f, 0.f, 0.f}}; // emission, rgba16f
	cv[3].depthStencil = {1.f, 0u}; // depth
	cv[4] = {{1000.f, 0.f, 0.f, 0.f}}; // linear r16f depth
	cv[5] = {{0.f, 0.f, 0.f, 0.f}}; // light, rgba16f
	cv[6] = {{0.f, 0.f, 0.f, 0.f}}; // reflectance, rgba16f
	cv[7] = {{1.f, 0.f, 0.f, 0.f}}; // revealage, r8unorm
	vk::cmdBeginRenderPass(cb, {rp_, fb_,
		{0u, 0u, width, height},
		std::uint32_t(cv.size()), cv.data()
	}, {});

	{
		vpp::DebugLabel(cb, "geometry pass");
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, geomPipe_);
		doi::cmdBindGraphicsDescriptors(cb, geomPipeLayout_, 0, {sceneDs});
		scene.renderOpaque(cb, geomPipeLayout_);
		time.add("geometry");
	}

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

	{
		vpp::DebugLabel(cb, "light pass");
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

		time.add("light");
		if(renderAO()) {
			vpp::DebugLabel(cb, "ao pass");
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, aoPipe_);
			doi::cmdBindGraphicsDescriptors(cb, aoPipeLayout_, 0, {sceneDs, aoDs_});
			vk::cmdPushConstants(cb, aoPipeLayout_, vk::ShaderStageBits::fragment,
				0, 4, &aoEnvLods_);
			vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad
			time.add("ao");
		}

		// important that this comes before the transparent pass since
		// that doesn't write the depth buffer
		if(env) {
			vk::cmdBindIndexBuffer(cb, boxIndices.buffer(),
				boxIndices.offset(), vk::IndexType::uint16);
			env->render(cb);
			time.add("env");
		}
	}

	vk::cmdNextSubpass(cb, vk::SubpassContents::eInline);

	{
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, transparentPipe_);
		doi::cmdBindGraphicsDescriptors(cb, geomPipeLayout_, 0, {sceneDs});
		scene.renderBlend(cb, geomPipeLayout_);
		time.add("transparent");
	}

	vk::cmdNextSubpass(cb, vk::SubpassContents::eInline);

	{
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, blendPipe_);
		doi::cmdBindGraphicsDescriptors(cb, blendPipeLayout_, 0, {blendDs_});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen tri fan
		time.add("blend");
	}

	vk::cmdEndRenderPass(cb);
}

void GeomLightPass::updateDevice() {
	if(renderAO()) {
		auto span = aoUboMap_.span();
		doi::write(span, aoParams);
		aoUboMap_.flush();
	}
}

// SyncScope GeomLightPass::srcScopeLight() const {
// 	return {
// 		vk::PipelineStageBits::colorAttachmentOutput,
// 		vk::ImageLayout::shaderReadOnlyOptimal,
// 		vk::AccessBits::colorAttachmentRead |
// 			vk::AccessBits::colorAttachmentWrite;
// 	};
// }

