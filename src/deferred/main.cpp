#include <stage/app.hpp>
#include <stage/camera.hpp>
#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>
#include <stage/scene/shape.hpp>
#include <stage/scene/light.hpp>
#include <stage/scene/scene.hpp>
#include <stage/scene/skybox.hpp>
#include <argagg.hpp>

#include <ny/appContext.hpp>
#include <ny/key.hpp>
#include <ny/mouseButton.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>

#include <shaders/stage.fullscreen.vert.h>
#include <shaders/deferred.gbuf.vert.h>
#include <shaders/deferred.gbuf.frag.h>
#include <shaders/deferred.pp.frag.h>
#include <shaders/deferred.pointLight.frag.h>
#include <shaders/deferred.dirLight.frag.h>

#include <cstdlib>
#include <random>

// TODO: re-implement ssao and light scattering

// NOTE: we always use ssao, even when object/material has ao texture.
// In that case both are multiplied. That's how its usually done, see
// https://docs.unrealengine.com/en-us/Engine/Rendering/LightingAndShadows/AmbientOcclusion

// NOTE: ssao currently independent from *any* lights at all
// NOTE: we currently completely ignore tangents and compute that
//   in the shader via derivates

// low prio optimizations:
// TODO(optimization?): we could theoretically just use one shadow map at all.
//   requires splitting the light passes though (rendering with light pipe)
//   so might have disadvantage. so not high prio
// TODO(optimization?) we could reuse float gbuffers for later hdr rendering
//   not that easy though since normal buffer has snorm format...
//   attachments can be used as color *and* input attachments in a
//   subpass.
// TODO(optimization): more efficient point light shadow cube map
//   rendering: only render those sides that we can actually see...
//   -> nothing culling-related implement at all at the moment
// NOTE: investigate/compare deferred lightning elements?
//   http://gameangst.com/?p=141


class ViewApp : public doi::App {
public:
	using Vertex = doi::Primitive::Vertex;

	static constexpr auto lightFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto normalsFormat = vk::Format::r16g16b16a16Snorm;
	static constexpr auto albedoFormat = vk::Format::r8g8b8a8Srgb;
	static constexpr auto emissionFormat = vk::Format::r8g8b8a8Unorm;
	static constexpr auto ssaoSampleCount = 64u;
	static constexpr auto pointLight = false;

	// see pp.frag
	struct PostProcessParams { // could name that PPP
		nytl::Vec3f scatterLightColor {1.f, 0.9f, 0.5f};
		std::uint32_t tonemap {2}; // uncharted
		float ssaoFactor {1.f};
		float exposure {1.f};
	};

public:
	bool init(const nytl::Span<const char*> args) override;
	void initRenderData() override;
	void initGPass();
	void initLPass();
	void initPPass();
	void initSSAO();

	vpp::ViewableImage createDepthTarget(const vk::Extent2D& size) override;
	void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) override;
	bool features(vk::PhysicalDeviceFeatures& enable,
		const vk::PhysicalDeviceFeatures& supported) override;
	argagg::parser argParser() const override;
	bool handleArgs(const argagg::parser_results& result) override;
	void record(const RenderBuffer& buffer) override;
	void update(double dt) override;
	bool key(const ny::KeyEvent& ev) override;
	void mouseMove(const ny::MouseMoveEvent& ev) override;
	bool mouseButton(const ny::MouseButtonEvent& ev) override;
	void resize(const ny::SizeEvent& ev) override;
	void updateDevice() override;

	vpp::RenderPass createRenderPass() override { return {}; } // we use our own
	bool needsDepth() const override { return true; }
	std::optional<unsigned> rvgSubpass() const override { return {}; }
	const char* name() const override { return "Deferred Renderer"; }

protected:
	vpp::TrDsLayout sceneDsLayout_;
	vpp::TrDsLayout primitiveDsLayout_;
	vpp::TrDsLayout materialDsLayout_;
	vpp::TrDsLayout lightDsLayout_;

	// light and shadow
	doi::ShadowData shadowData_;
	std::vector<doi::DirLight> dirLights_;
	std::vector<doi::PointLight> pointLights_;

	vpp::SubBuffer sceneUbo_;
	vpp::TrDs sceneDs_;

	vpp::ViewableImage dummyTex_;
	std::unique_ptr<doi::Scene> scene_;
	std::optional<doi::Material> lightMaterial_;

	std::string modelname_ {};
	float sceneScale_ {1.f};

	bool anisotropy_ {}; // whether device supports anisotropy
	std::uint32_t show_ {}; // what to show
	float time_ {};
	bool rotateView_ {}; // mouseLeft down
	doi::Camera camera_ {};
	bool updateLights_ {true};

	// doi::Skybox skybox_;

	// image view into the depth buffer that accesses all depth levels
	vpp::ImageView depthMipView_;
	unsigned depthMipLevels_ {};

	// gbuffer and lightning passes
	struct {
		vpp::RenderPass pass;
		vpp::Framebuffer fb;
		vpp::ViewableImage normal;
		vpp::ViewableImage albedo;
		vpp::ViewableImage emission;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} gpass_;

	// lightning pass
	struct {
		vpp::RenderPass pass;
		vpp::Framebuffer fb;
		vpp::ViewableImage light;
		vpp::TrDsLayout dsLayout; // input
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pointPipe;
		vpp::Pipeline dirPipe;
	} lpass_;

	// post processing
	struct {
		vpp::RenderPass pass;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::SubBuffer ubo;
		vpp::Pipeline pipe;

		PostProcessParams params;
		bool updateParams {true};
	} pp_;

	// ssao
	struct {
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::ViewableImage target;
		vpp::ViewableImage noise;
		vpp::SubBuffer samples;
		unsigned numSamples = 32u;
	} ssao_;
};

bool ViewApp::init(const nytl::Span<const char*> args) {
	if(!doi::App::init(args)) {
		return false;
	}

	auto& dev = vulkanDevice();
	camera_.perspective.near = 0.1f;
	camera_.perspective.far = 100.f;

	// TODO: re-add this. In pass between light and pp?
	// hdr skybox
	// skybox_.init(dev, "../assets/kloofendal2k.hdr",
		// renderPass(), 3u, samples());

	// Load Model
	auto s = sceneScale_;
	auto mat = doi::scaleMat<4, float>({s, s, s});
	auto samplerAnisotropy = 1.f;
	if(anisotropy_) {
		samplerAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
	}

	auto ri = doi::SceneRenderInfo{
		materialDsLayout_,
		primitiveDsLayout_,
		dummyTex_.vkImageView(),
		samplerAnisotropy
	};
	if(!(scene_ = doi::loadGltf(modelname_, dev, mat, ri))) {
		return false;
	}

	lightMaterial_.emplace(materialDsLayout_,
		dummyTex_.vkImageView(), scene_->defaultSampler(),
		nytl::Vec{1.f, 1.f, 0.4f, 1.f});

	if(pointLight) {
		auto& l = pointLights_.emplace_back(dev, lightDsLayout_,
			primitiveDsLayout_, shadowData_, *lightMaterial_);
		l.data.position = {-1.8f, 6.0f, -2.f};
		l.data.color = {5.f, 4.f, 2.f};
		l.data.attenuation = {1.f, 0.4f, 0.2f};
		l.updateDevice();
	} else {
		auto& l = dirLights_.emplace_back(dev, lightDsLayout_,
			primitiveDsLayout_, shadowData_, camera_.pos, *lightMaterial_);
		l.data.dir = {-3.8f, -9.2f, -5.2f};
		l.data.color = {5.f, 4.f, 2.f};
		l.updateDevice(camera_.pos);
	}

	return true;
}

void ViewApp::initRenderData() {
	auto& dev = vulkanDevice();

	initGPass(); // pass 0.0: geometry to g buffers
	initLPass(); // pass 1.0: per light: using g buffers for shading
	initPPass(); // pass 2.0: post processing, combining

	initSSAO();

	// dummy texture for materials that don't have a texture
	std::array<std::uint8_t, 4> data{255u, 255u, 255u, 255u};
	auto ptr = reinterpret_cast<std::byte*>(data.data());
	dummyTex_ = doi::loadTexture(dev, {1, 1, 1},
		vk::Format::r8g8b8a8Unorm, {ptr, data.size()});

	// ubo and stuff
	auto sceneUboSize = sizeof(nytl::Mat4f) // proj matrix
		+ sizeof(nytl::Mat4f) // inv proj
		+ sizeof(nytl::Vec3f); // viewPos
	sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
	sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
		vk::BufferUsageBits::uniformBuffer, 0, dev.hostMemoryTypes()};

	shadowData_ = doi::initShadowData(dev, depthFormat(),
		lightDsLayout_, materialDsLayout_, primitiveDsLayout_,
		doi::Material::pcr());

	// scene descriptor, used for some pipelines as set 0 for camera
	// matrix and view position
	vpp::DescriptorSetUpdate sdsu(sceneDs_);
	sdsu.uniform({{sceneUbo_.buffer(), sceneUbo_.offset(), sceneUbo_.size()}});
	vpp::apply({sdsu});
}

void ViewApp::initGPass() {
	auto& dev = vulkanDevice();

	// render pass
	std::array<vk::AttachmentDescription, 4> attachments;
	struct {
		unsigned normals = 0;
		unsigned albedo = 1;
		unsigned emission = 2;
		unsigned depth = 3;
	} ids;

	auto addGBuf = [&](auto id, auto format) {
		attachments[id].format = format;
		attachments[id].samples = samples();
		attachments[id].loadOp = vk::AttachmentLoadOp::clear;
		attachments[id].storeOp = vk::AttachmentStoreOp::store;
		attachments[id].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[id].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[id].initialLayout = vk::ImageLayout::undefined;
		attachments[id].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	};

	addGBuf(ids.normals, normalsFormat);
	addGBuf(ids.albedo, albedoFormat);
	addGBuf(ids.emission, emissionFormat);
	addGBuf(ids.depth, depthFormat());

	// NOTE: we do this here since after the gpass we first create mipmaps
	// levels of the depth buffer since for the multiple algorithms that
	// use it for sampling (ssao, light scattering, ssr)
	attachments[ids.depth].finalLayout = vk::ImageLayout::transferSrcOptimal;

	// subpass
	vk::AttachmentReference gbufRefs[3];
	gbufRefs[0].attachment = ids.normals;
	gbufRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[1].attachment = ids.albedo;
	gbufRefs[1].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[2].attachment = ids.emission;
	gbufRefs[2].layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference depthRef;
	depthRef.attachment = ids.depth;
	depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 3;
	subpass.pColorAttachments = gbufRefs;
	subpass.pDepthStencilAttachment = &depthRef;

	// since there follow other passes that read from all the gbuffers
	// we have to declare an external dependency.
	// The source is the gbuffer writes (and depth, included in lateFragTests).
	// The destination is the reading of the gbuffer in a compute or fragment
	// shader or re-using the depth attachment.
	vk::SubpassDependency dependency;
	dependency.srcSubpass = 0u;
	dependency.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput |
		vk::PipelineStageBits::lateFragmentTests;
	dependency.srcAccessMask = vk::AccessBits::colorAttachmentWrite |
		vk::AccessBits::depthStencilAttachmentWrite;
	dependency.dstSubpass = vk::subpassExternal;
	dependency.dstStageMask = vk::PipelineStageBits::computeShader |
		vk::PipelineStageBits::fragmentShader |
		vk::PipelineStageBits::earlyFragmentTests |
		vk::PipelineStageBits::lateFragmentTests;
	dependency.dstAccessMask = vk::AccessBits::inputAttachmentRead |
		vk::AccessBits::depthStencilAttachmentRead |
		vk::AccessBits::depthStencilAttachmentWrite |
		vk::AccessBits::shaderRead;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.dependencyCount = 1u;
	rpi.pDependencies = &dependency;
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	gpass_.pass = {dev, rpi};


	// pipeline
	// per scene; view + projection matrix
	auto sceneBindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
	};

	sceneDsLayout_ = {dev, sceneBindings};

	// per object; model matrix and material stuff
	auto objectBindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
	};

	primitiveDsLayout_ = {dev, objectBindings};

	// per material
	// push constant range for material
	materialDsLayout_ = doi::Material::createDsLayout(dev);
	auto pcr = doi::Material::pcr();

	gpass_.pipeLayout = {dev, {
		sceneDsLayout_,
		materialDsLayout_,
		primitiveDsLayout_,
	}, {pcr}};

	vpp::ShaderModule vertShader(dev, deferred_gbuf_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_gbuf_frag_data);
	vpp::GraphicsPipelineInfo gpi {gpass_.pass, gpass_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}, 0};

	constexpr auto stride = sizeof(Vertex);
	vk::VertexInputBindingDescription bufferBindings[2] = {
		{0, stride, vk::VertexInputRate::vertex},
		{1, sizeof(float) * 2, vk::VertexInputRate::vertex} // uv
	};

	vk::VertexInputAttributeDescription attributes[3] {};
	attributes[0].format = vk::Format::r32g32b32Sfloat; // pos

	attributes[1].format = vk::Format::r32g32b32Sfloat; // normal
	attributes[1].offset = sizeof(float) * 3; // pos
	attributes[1].location = 1;

	attributes[2].format = vk::Format::r32g32Sfloat; // uv
	attributes[2].location = 2;
	attributes[2].binding = 1;

	// we don't blend in the gbuffers; simply overwrite
	vk::PipelineColorBlendAttachmentState blendAttachments[3];
	blendAttachments[0].blendEnable = false;
	blendAttachments[0].colorWriteMask =
		vk::ColorComponentBits::r |
		vk::ColorComponentBits::g |
		vk::ColorComponentBits::b |
		vk::ColorComponentBits::a;

	blendAttachments[1] = blendAttachments[0];
	blendAttachments[2] = blendAttachments[0];

	gpi.blend.attachmentCount = 3u;
	gpi.blend.pAttachments = blendAttachments;

	gpi.vertex.pVertexAttributeDescriptions = attributes;
	gpi.vertex.vertexAttributeDescriptionCount = 3u;
	gpi.vertex.pVertexBindingDescriptions = bufferBindings;
	gpi.vertex.vertexBindingDescriptionCount = 2u;
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

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {},
		1, gpi.info(), NULL, vkpipe);

	gpass_.pipe = {dev, vkpipe};
}

void ViewApp::initLPass() {
	auto& dev = vulkanDevice();

	std::array<vk::AttachmentDescription, 5> attachments;
	struct {
		unsigned normals = 0;
		unsigned albedo = 1;
		unsigned emission = 2;
		unsigned depth = 3;
		unsigned light = 4;
	} ids;

	// light
	attachments[ids.light].format = lightFormat;
	attachments[ids.light].samples = samples();
	attachments[ids.light].loadOp = vk::AttachmentLoadOp::clear;
	attachments[ids.light].storeOp = vk::AttachmentStoreOp::store;
	attachments[ids.light].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[ids.light].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[ids.light].initialLayout = vk::ImageLayout::undefined;
	attachments[ids.light].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	auto addGBuf = [&](auto id, auto format) {
		attachments[id].format = format;
		attachments[id].samples = samples();
		attachments[id].loadOp = vk::AttachmentLoadOp::load;
		attachments[id].storeOp = vk::AttachmentStoreOp::store;
		attachments[id].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[id].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[id].initialLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[id].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	};

	addGBuf(ids.normals, normalsFormat);
	addGBuf(ids.albedo, albedoFormat);
	addGBuf(ids.emission, emissionFormat);
	addGBuf(ids.depth, depthFormat());

	// subpass
	vk::AttachmentReference gbufRefs[4];
	gbufRefs[0].attachment = ids.normals;
	gbufRefs[0].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	gbufRefs[1].attachment = ids.albedo;
	gbufRefs[1].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	gbufRefs[2].attachment = ids.emission;
	gbufRefs[2].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	gbufRefs[3].attachment = ids.depth;
	gbufRefs[3].layout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::AttachmentReference lightRef;
	lightRef.attachment = ids.light;
	lightRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &lightRef;
	subpass.inputAttachmentCount = 4u;
	subpass.pInputAttachments = gbufRefs;

	// since we sample from the light buffer in future passes,
	// we need to insert a dependency making sure that writing
	// it finished after this subpass.
	vk::SubpassDependency dependency;
	dependency.srcSubpass = 0u;
	dependency.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	dependency.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	dependency.dstSubpass = vk::subpassExternal;
	dependency.dstStageMask = vk::PipelineStageBits::computeShader |
		vk::PipelineStageBits::fragmentShader;
	dependency.dstAccessMask = vk::AccessBits::inputAttachmentRead |
		vk::AccessBits::shaderRead;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;
	rpi.dependencyCount = 1u;
	rpi.pDependencies = &dependency;

	lpass_.pass = {dev, rpi};

	// pipeline
	// gbuffer input ds
	auto inputBindings = {
		vpp::descriptorBinding( // normal
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // albedo
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // emission
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // depth
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
	};

	lpass_.dsLayout = {dev, inputBindings};
	lpass_.ds = {device().descriptorAllocator(), lpass_.dsLayout};

	// light ds
	// TODO: statically use shadow data sampler here?
	// there is no real reason not to... expect maybe dir and point
	// lights using different samplers? look into that
	auto lightBindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment),
	};
	lightDsLayout_ = {dev, lightBindings};

	// light pipe
	vk::PushConstantRange pcr {};
	pcr.size = 4;
	pcr.stageFlags = vk::ShaderStageBits::fragment;
	lpass_.pipeLayout = {dev, {sceneDsLayout_, lpass_.dsLayout, lightDsLayout_},
		{pcr}};

	// TODO: don't use fullscreen here. Only render the areas of the screen
	// that are effected by the light (huge, important optimization
	// when there are many lights in the scene!)
	// Render simple box volume for best performance
	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule pointFragShader(dev, deferred_pointLight_frag_data);
	vpp::GraphicsPipelineInfo pgpi{lpass_.pass, lpass_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{pointFragShader, vk::ShaderStageBits::fragment},
	}}};

	pgpi.depthStencil.depthTestEnable = false;
	pgpi.depthStencil.depthWriteEnable = false;
	pgpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

	// additive blending
	vk::PipelineColorBlendAttachmentState blendAttachment;
	blendAttachment.blendEnable = true;
	blendAttachment.colorBlendOp = vk::BlendOp::add;
	blendAttachment.srcColorBlendFactor = vk::BlendFactor::one;
	blendAttachment.dstColorBlendFactor = vk::BlendFactor::one;
	blendAttachment.alphaBlendOp = vk::BlendOp::add;
	blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::one;
	blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::one;
	blendAttachment.colorWriteMask =
		vk::ColorComponentBits::r |
		vk::ColorComponentBits::g |
		vk::ColorComponentBits::b |
		vk::ColorComponentBits::a;

	pgpi.blend.attachmentCount = 1u;
	pgpi.blend.pAttachments = &blendAttachment;

	// dir light
	// here we can use a fullscreen shader pass since directional lights
	// don't have a light volume
	vpp::ShaderModule dirFragShader(dev, deferred_dirLight_frag_data);
	vpp::GraphicsPipelineInfo lgpi{lpass_.pass, lpass_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{dirFragShader, vk::ShaderStageBits::fragment},
	}}};

	lgpi.blend = pgpi.blend;
	lgpi.depthStencil = pgpi.depthStencil;
	lgpi.assembly = pgpi.assembly;

	// create the pipes
	vk::GraphicsPipelineCreateInfo infos[] = {pgpi.info(), lgpi.info()};
	vk::Pipeline vkpipes[2];
	vk::createGraphicsPipelines(dev, {},
		2, *infos, NULL, *vkpipes);

	lpass_.pointPipe = {dev, vkpipes[0]};
	lpass_.dirPipe = {dev, vkpipes[1]};
}
void ViewApp::initPPass() {
	auto& dev = vulkanDevice();

	// render pass
	std::array<vk::AttachmentDescription, 3u> attachments;
	struct {
		unsigned swapchain = 0u;
		unsigned light = 1u;
		unsigned albedo = 2u;
	} ids;

	attachments[ids.swapchain].format = swapchainInfo().imageFormat;
	attachments[ids.swapchain].samples = samples();
	attachments[ids.swapchain].loadOp = vk::AttachmentLoadOp::dontCare;
	attachments[ids.swapchain].storeOp = vk::AttachmentStoreOp::store;
	attachments[ids.swapchain].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[ids.swapchain].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[ids.swapchain].initialLayout = vk::ImageLayout::undefined;
	attachments[ids.swapchain].finalLayout = vk::ImageLayout::presentSrcKHR;

	auto addInput = [&](auto id, auto format) {
		attachments[id].format = format;
		attachments[id].samples = samples();
		attachments[id].loadOp = vk::AttachmentLoadOp::load;
		attachments[id].storeOp = vk::AttachmentStoreOp::store;
		attachments[id].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[id].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[id].initialLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[id].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	};

	addInput(ids.light, lightFormat);
	addInput(ids.albedo, albedoFormat);

	// subpass
	vk::AttachmentReference inputRefs[2];
	inputRefs[0].attachment = ids.light;
	inputRefs[0].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	inputRefs[1].attachment = ids.albedo;
	inputRefs[1].layout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::AttachmentReference colorRef;
	colorRef.attachment = ids.swapchain;
	colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;
	subpass.inputAttachmentCount = 2u;
	subpass.pInputAttachments = inputRefs;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	pp_.pass = {dev, rpi};

	// pipe
	auto lightInputBindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment), // light output
		vpp::descriptorBinding(
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment), // albedo for ao
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment), // ubo
	};

	pp_.dsLayout = {dev, lightInputBindings};
	pp_.ds = {device().descriptorAllocator(), pp_.dsLayout};

	auto uboSize = sizeof(PostProcessParams);
	pp_.ubo = {dev.bufferAllocator(), uboSize,
		vk::BufferUsageBits::uniformBuffer, 0, dev.hostMemoryTypes()};

	pp_.pipeLayout = {dev, {pp_.dsLayout}, {}};

	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_pp_frag_data);
	vpp::GraphicsPipelineInfo gpi {pp_.pass, pp_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}};

	gpi.depthStencil.depthTestEnable = false;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {},
		1, gpi.info(), NULL, vkpipe);

	pp_.pipe = {dev, vkpipe};
}

void ViewApp::initSSAO() {
	/*
	auto ssaoBindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
	};

	ssaoDsLayout_ = {dev, ssaoBindings};

	// ssao
	std::default_random_engine rndEngine;
	std::uniform_real_distribution<float> rndDist(0.0f, 1.0f);

	// Sample kernel
	std::vector<nytl::Vec4f> ssaoKernel(SSAO_SAMPLE_COUNT);
	for(auto i = 0u; i < SSAO_SAMPLE_COUNT; ++i) {
		nytl::Vec3f sample{
			2.f * rndDist(rndEngine) - 1.f,
			2.f * rndDist(rndEngine) - 1.f,
			rndDist(rndEngine)};
		sample = normalized(sample);
		sample *= rndDist(rndEngine);
		float scale = float(i) / float(SSAO_SAMPLE_COUNT);
		scale = nytl::mix(0.1f, 1.0f, scale * scale);
		ssaoKernel[i] = nytl::Vec4f(scale * sample);
	}

	// ubo
	auto devMem = dev.deviceMemoryTypes();
	auto size = sizeof(nytl::Vec4f) * SSAO_SAMPLE_COUNT;
	auto usage = vk::BufferUsageBits::transferDst |
		vk::BufferUsageBits::uniformBuffer;
	ssaoSamples_ = {dev.bufferAllocator(), size, usage, 0, devMem};
	vpp::writeStaging140(ssaoSamples_, vpp::raw(*ssaoKernel.data(),
			ssaoKernel.size()));

	// NOTE: we could use a r32g32f format, would be more efficent
	// might not be supported though... we could pack it into somehow
	auto ssaoNoiseDim = 4u;
	std::vector<nytl::Vec4f> ssaoNoise(ssaoNoiseDim * ssaoNoiseDim);
	for(auto i = 0u; i < static_cast<uint32_t>(ssaoNoise.size()); i++) {
		ssaoNoise[i] = nytl::Vec4f{
			rndDist(rndEngine) * 2.f - 1.f,
			rndDist(rndEngine) * 2.f - 1.f,
			0.0f, 0.0f
		};
	}

	auto ptr = reinterpret_cast<const std::byte*>(ssaoNoise.data());
	auto ptrSize = ssaoNoise.size() * sizeof(ssaoNoise[0]);
	ssaoNoise_ = doi::loadTexture(dev, {ssaoNoiseDim, ssaoNoiseDim, 1u},
		vk::Format::r32g32b32a32Sfloat, {ptr, ptrSize});

	// ds
	ssaoDs_ = {dev.descriptorAllocator(), ssaoDsLayout_};
	vpp::DescriptorSetUpdate dsu(ssaoDs_);
	dsu.uniform({{ssaoSamples_.buffer(), ssaoSamples_.offset(),
		ssaoSamples_.size()}});
	dsu.imageSampler({{{}, ssaoNoise_.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	*/
}

vpp::ViewableImage ViewApp::createDepthTarget(const vk::Extent2D& size) {
	auto width = size.width;
	auto height = size.height;
	auto levels = 1 + std::floor(std::log2(std::max(width, height)));

	// img
	vk::ImageCreateInfo img;
	img.imageType = vk::ImageType::e2d;
	img.format = depthFormat();
	img.extent.width = width;
	img.extent.height = height;
	img.extent.depth = 1;
	img.mipLevels = levels;
	img.arrayLayers = 1;
	img.sharingMode = vk::SharingMode::exclusive;
	img.tiling = vk::ImageTiling::optimal;
	img.samples = samples();
	img.usage = vk::ImageUsageBits::depthStencilAttachment |
		vk::ImageUsageBits::inputAttachment |
		vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst;
	img.initialLayout = vk::ImageLayout::undefined;

	// view
	vk::ImageViewCreateInfo view;
	view.viewType = vk::ImageViewType::e2d;
	view.format = img.format;
	view.components = {};
	view.subresourceRange.aspectMask = vk::ImageAspectBits::depth;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;

	// create the viewable image
	// will set the created image in the view info for us
	vpp::ViewableImage target = {device(), img, view};

	// view.subresourceRange.levelCount = levels;
	view.image = target.image();
	depthMipView_ = {device(), view};
	depthMipLevels_ = levels;

	return target;
}

void ViewApp::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	// depth
	auto scPos = 0u; // attachments[scPos]: swapchain image
	depthTarget() = createDepthTarget(size);

	std::vector<vk::ImageView> attachments;
	auto initBuf = [&](vpp::ViewableImage& img, vk::Format format) {
		auto usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::inputAttachment |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo::color(device(),
			{size.width, size.height}, usage, {format},
			vk::ImageTiling::optimal, samples());
		dlg_assert(info);
		img = {device(), *info};
		attachments.push_back(img.vkImageView());
	};

	// create offscreen buffers, gbufs
	initBuf(gpass_.normal, normalsFormat);
	initBuf(gpass_.albedo, albedoFormat);
	initBuf(gpass_.emission, emissionFormat);
	attachments.push_back(depthTarget().vkImageView()); // depth
	vk::FramebufferCreateInfo fbi({}, gpass_.pass,
		attachments.size(), attachments.data(),
		size.width, size.height, 1);
	gpass_.fb = {device(), fbi};

	// light buf
	initBuf(lpass_.light, lightFormat);
	fbi = vk::FramebufferCreateInfo({}, lpass_.pass,
		attachments.size(), attachments.data(),
		size.width, size.height, 1);
	lpass_.fb = {device(), fbi};

	// create swapchain framebuffers
	attachments.clear();
	attachments.emplace_back(); // scPos
	attachments.push_back(lpass_.light.vkImageView());
	attachments.push_back(gpass_.albedo.vkImageView());

	for(auto& buf : bufs) {
		attachments[scPos] = buf.imageView;
		vk::FramebufferCreateInfo fbi({}, pp_.pass,
			attachments.size(), attachments.data(),
			size.width, size.height, 1);
		buf.framebuffer = {device(), fbi};
	}

	// update descriptor sets
	vpp::DescriptorSetUpdate dsu(lpass_.ds);
	dsu.inputAttachment({{{}, gpass_.normal.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.inputAttachment({{{}, gpass_.albedo.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.inputAttachment({{{}, gpass_.emission.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.inputAttachment({{{}, depthMipView_,
		vk::ImageLayout::shaderReadOnlyOptimal}});

	vpp::DescriptorSetUpdate ldsu(pp_.ds);
	ldsu.inputAttachment({{{}, lpass_.light.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.inputAttachment({{{}, gpass_.albedo.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.uniform({{pp_.ubo.buffer(), pp_.ubo.offset(), pp_.ubo.size()}});

	vpp::apply({dsu, ldsu});
}

// enable anisotropy if possible
bool ViewApp::features(vk::PhysicalDeviceFeatures& enable,
		const vk::PhysicalDeviceFeatures& supported) {
	if(supported.samplerAnisotropy) {
		anisotropy_ = true;
		enable.samplerAnisotropy = true;
	}

	return true;
}

argagg::parser ViewApp::argParser() const {
	// msaa not supported in deferred renderer
	auto parser = App::argParser();
	auto& defs = parser.definitions;
	auto it = std::find_if(defs.begin(), defs.end(),
		[](const argagg::definition& def){
			return def.name == "multisamples";
	});
	dlg_assert(it != defs.end());
	defs.erase(it);

	defs.push_back({
		"model", {"--model"},
		"Path of the gltf model to load (dir must contain scene.gltf)", 1
	});
	defs.push_back({
		"scale", {"--scale"},
		"Apply scale to whole scene", 1
	});
	return parser;
}

bool ViewApp::handleArgs(const argagg::parser_results& result) {
	if(!App::handleArgs(result)) {
		return false;
	}

	if(result.has_option("model")) {
		modelname_ = result["model"].as<const char*>();
	}
	if(result.has_option("scale")) {
		sceneScale_ = result["scale"].as<float>();
	}

	return true;
}

void ViewApp::record(const RenderBuffer& buf) {
	auto cb = buf.commandBuffer;
	vk::beginCommandBuffer(cb, {});
	App::beforeRender(cb);

	// render shadow maps
	for(auto& light : pointLights_) {
		light.render(cb, shadowData_, *scene_);
	}
	for(auto& light : dirLights_) {
		light.render(cb, shadowData_, *scene_);
	}

	const auto width = swapchainInfo().imageExtent.width;
	const auto height = swapchainInfo().imageExtent.height;

	// gpass
	{
		std::array<vk::ClearValue, 4u> cv {};
		cv[0] = cv[1] = cv[2] = {{0.f, 0.f, 0.f, 0.f}};
		cv[3].depthStencil = {1.f, 0u};
		vk::cmdBeginRenderPass(cb, {
			gpass_.pass,
			gpass_.fb,
			{0u, 0u, width, height},
			std::uint32_t(cv.size()), cv.data()
		}, {});

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gpass_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gpass_.pipeLayout, 0, {sceneDs_}, {});
		scene_->render(cb, gpass_.pipeLayout);

		// NOTE: ideally, don't render these in gbuffer pass but later
		// on with different lightning?
		// for(auto& l : pointLights_) {
		// 	l.lightBall().render(cb, gPipeLayout_);
		// }
		// for(auto& l : dirLights_) {
		// 	l.lightBall().render(cb, gPipeLayout_);
		// }

		vk::cmdEndRenderPass(cb);
	}

	// create depth mipmaps
	// the depth target already is in transferSrcOptimal layout
	// from the last render pass
	auto& depthImg = depthTarget().image();

	// transition all but mipmap level 0 to transferDst
	vpp::changeLayout(cb, depthImg,
		vk::ImageLayout::undefined, // we don't need the content any more
		vk::PipelineStageBits::topOfPipe, {},
		vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer,
		vk::AccessBits::transferWrite,
		{vk::ImageAspectBits::depth, 1, depthMipLevels_ - 1, 0, 1});

	for(auto i = 1u; i < depthMipLevels_; ++i) {
		// std::max needed for end offsets when the texture is not
		// quadratic: then we would get 0 there although the mipmap
		// still has size 1
		vk::ImageBlit blit;
		blit.srcSubresource.layerCount = 1;
		blit.srcSubresource.mipLevel = i - 1;
		blit.srcSubresource.aspectMask = vk::ImageAspectBits::depth;
		blit.srcOffsets[1].x = std::max(width >> (i - 1), 1u);
		blit.srcOffsets[1].y = std::max(height >> (i - 1), 1u);
		blit.srcOffsets[1].z = 1u;

		blit.dstSubresource.layerCount = 1;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.aspectMask = vk::ImageAspectBits::depth;
		blit.dstOffsets[1].x = std::max(width >> i, 1u);
		blit.dstOffsets[1].y = std::max(height >> i, 1u);
		blit.dstOffsets[1].z = 1u;

		vk::cmdBlitImage(cb, depthImg, vk::ImageLayout::transferSrcOptimal,
			depthImg, vk::ImageLayout::transferDstOptimal, {blit},
			vk::Filter::nearest);

		// change layout of current mip level to transferSrc for next mip level
		vpp::changeLayout(cb, depthImg,
			vk::ImageLayout::transferDstOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferWrite,
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			{vk::ImageAspectBits::depth, i, 1, 0, 1});
	}

	// transform all levels back to readonly
	vpp::changeLayout(cb, depthImg,
		vk::ImageLayout::transferSrcOptimal,
		vk::PipelineStageBits::transfer,
		vk::AccessBits::transferRead,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::allCommands,
		vk::AccessBits::shaderRead,
		{vk::ImageAspectBits::depth, 0, depthMipLevels_, 0, 1});

	// lpass
	{
		std::array<vk::ClearValue, 5u> cv {};
		cv[4] = {{0.f, 0.f, 0.f, 0.f}};
		vk::cmdBeginRenderPass(cb, {
			lpass_.pass,
			lpass_.fb,
			{0u, 0u, width, height},
			std::uint32_t(cv.size()), cv.data()
		}, {});

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		// make make this a uniform buffer?
		vk::cmdPushConstants(cb, lpass_.pipeLayout,
			vk::ShaderStageBits::fragment, 0, 4, &show_);

		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			lpass_.pipeLayout, 0, {sceneDs_, lpass_.ds}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, lpass_.pointPipe);
		for(auto& light : pointLights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				lpass_.pipeLayout, 2, {light.ds()}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		}

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, lpass_.dirPipe);
		for(auto& light : dirLights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				lpass_.pipeLayout, 2, {light.ds()}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		}

		vk::cmdEndRenderPass(cb);
	}

	// post process pass
	{
		vk::cmdBeginRenderPass(cb, {
			pp_.pass,
			buf.framebuffer,
			{0u, 0u, width, height},
			0, nullptr
		}, {});

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pp_.pipeLayout, 0, {pp_.ds}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen

		vk::cmdEndRenderPass(cb);
	}

	App::afterRender(cb);
	vk::endCommandBuffer(cb);
}

void ViewApp::update(double dt) {
	App::update(dt);
	time_ += dt;

	// movement
	auto kc = appContext().keyboardContext();
	if(kc) {
		doi::checkMovement(camera_, *kc, dt);
	}

	if(camera_.update || updateLights_) {
		App::scheduleRedraw();
	}

	// TODO: only here for fps testing
	App::scheduleRedraw();
}

bool ViewApp::key(const ny::KeyEvent& ev) {
	if(App::key(ev)) {
		return true;
	}

	if(!ev.pressed) {
		return false;
	}

	auto numModes = 9u;
	switch(ev.keycode) {
		case ny::Keycode::m: // move light here
			if(!dirLights_.empty()) {
				dirLights_[0].data.dir = -camera_.pos;
			} else {
				pointLights_[0].data.position = camera_.pos;
			}
			updateLights_ = true;
			return true;
		case ny::Keycode::p:
			if(!dirLights_.empty()) {
				dirLights_[0].data.flags ^= doi::lightFlagPcf;
			} else {
				pointLights_[0].data.flags ^= doi::lightFlagPcf;
			}
			updateLights_ = true;
			return true;
		case ny::Keycode::n:
			show_ = (show_ + 1) % numModes;
			App::scheduleRerecord();
			return true;
		case ny::Keycode::l:
			show_ = (show_ + numModes - 1) % numModes;
			App::scheduleRerecord();
			return true;
		case ny::Keycode::i:
			pp_.params.ssaoFactor = 1.f - pp_.params.ssaoFactor;
			pp_.updateParams = true;
			return true;
		case ny::Keycode::equals:
			pp_.params.exposure *= 1.1f;
			pp_.updateParams = true;
			return true;
		case ny::Keycode::minus:
			pp_.params.exposure /= 1.1f;
			pp_.updateParams = true;
			return true;
		default:
			break;
	}

	return false;
}

void ViewApp::mouseMove(const ny::MouseMoveEvent& ev) {
	App::mouseMove(ev);
	if(rotateView_) {
		doi::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
		App::scheduleRedraw();
	}
}

bool ViewApp::mouseButton(const ny::MouseButtonEvent& ev) {
	if(App::mouseButton(ev)) {
		return true;
	}

	if(ev.button == ny::MouseButton::left) {
		rotateView_ = ev.pressed;
		return true;
	}

	return false;
}

void ViewApp::updateDevice() {
	// update scene ubo
	if(camera_.update) {
		camera_.update = false;
		auto map = sceneUbo_.memoryMap();
		auto span = map.span();
		auto mat = matrix(camera_);
		doi::write(span, mat);
		doi::write(span, nytl::Mat4f(nytl::inverse(mat)));
		doi::write(span, camera_.pos);

		// skybox_.updateDevice(fixedMatrix(camera_));

		// depend on camera position
		for(auto& l : dirLights_) {
			l.updateDevice(camera_.pos);
		}
	}

	if(updateLights_) {
		for(auto& l : pointLights_) {
			l.updateDevice();
		}
		for(auto& l : dirLights_) {
			l.updateDevice(camera_.pos);
		}
		updateLights_ = false;
	}

	if(pp_.updateParams) {
		auto map = pp_.ubo.memoryMap();
		auto span = map.span();
		doi::write(span, pp_.params);
	}
}

void ViewApp::resize(const ny::SizeEvent& ev) {
	App::resize(ev);
	camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
	camera_.update = true;
}

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({*argv, std::size_t(argc)})) {
		return EXIT_FAILURE;
	}

	app.run();
}
