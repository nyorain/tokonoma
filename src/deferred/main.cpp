#include "light.hpp"

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

#include <shaders/deferred.gbuf.vert.h>
#include <shaders/deferred.gbuf.frag.h>
#include <shaders/fullscreen.vert.h>
#include <shaders/deferred.light.frag.h>

#include <cstdlib>

// TODO: no hdr at the moment. Will look shitty for multiple lights

class GRenderer : public doi::Renderer {
public:
	const vpp::TrDsLayout* inputDsLayout_ {};

public:
	GRenderer(const doi::RendererCreateInfo& ri);
	const auto& inputDs() const { return inputDs_; }

protected:
	void createRenderPass() override;
	void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) override;
	std::vector<vk::ClearValue> clearValues() override;

protected:
	vpp::TrDs inputDs_;

	// additional render targets
	vpp::ViewableImage pos_;
	vpp::ViewableImage albedo_;
	vpp::ViewableImage normal_;
};

class ViewApp : public doi::App {
public:
	using Vertex = doi::Primitive::Vertex;

public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto hostMem = dev.hostMemoryTypes();

		// renderer already queried the best supported depth format
		depthFormat_ = renderer().depthFormat();
		camera_.perspective.far = 100.f;

		// tex sampler
		vk::SamplerCreateInfo sci {};
		sci.addressModeU = vk::SamplerAddressMode::repeat;
		sci.addressModeV = vk::SamplerAddressMode::repeat;
		sci.addressModeW = vk::SamplerAddressMode::repeat;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.minLod = 0.0;
		sci.maxLod = 0.25;
		sci.mipmapMode = vk::SamplerMipmapMode::linear;
		sampler_ = {dev, sci};

		initGPipe();
		initLPipe();

		// dummy texture for materials that don't have a texture
		std::array<std::uint8_t, 4> data{255u, 255u, 255u, 255u};
		auto ptr = reinterpret_cast<std::byte*>(data.data());
		dummyTex_ = doi::loadTexture(dev, {1, 1, 1},
			vk::Format::r8g8b8a8Unorm, {ptr, data.size()});

		// Load Model
		auto s = sceneScale_;
		auto mat = doi::scaleMat<4, float>({s, s, s});
		auto ri = doi::SceneRenderInfo{materialDsLayout_, primitiveDsLayout_,
			gPipeLayout_, dummyTex_.vkImageView()};
		if(!(scene_ = doi::loadGltf(modelname_, dev, mat, ri))) {
			return false;
		}

		// ubo and stuff
		auto sceneUboSize = sizeof(nytl::Mat4f) // proj matrix
			+ sizeof(nytl::Vec3f); // viewPos
		sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
		sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		// example light
		shadowData_ = initShadowData(dev, gPipeLayout_, depthFormat_);
		auto& l = lights_.emplace_back(dev, lightDsLayout_, shadowData_);
		l.data.pd = {5.8f, 12.0f, 4.f};
		l.data.type = Light::Type::point;
		l.updateDevice();

		// descriptors
		vpp::DescriptorSetUpdate sdsu(sceneDs_);
		sdsu.uniform({{sceneUbo_.buffer(), sceneUbo_.offset(), sceneUbo_.size()}});
		vpp::apply({sdsu});

		return true;
	}

	void initGPipe() {
		auto& dev = vulkanDevice();

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
		materialDsLayout_ = doi::Material::createDsLayout(dev, sampler_);
		vk::PushConstantRange pcr;
		pcr.offset = 0;
		pcr.size = sizeof(float) * 8;
		pcr.stageFlags = vk::ShaderStageBits::fragment;

		gPipeLayout_ = {dev, {
			sceneDsLayout_,
			materialDsLayout_,
			primitiveDsLayout_,
		}, {pcr}};

		vpp::ShaderModule vertShader(dev, deferred_gbuf_vert_data);
		vpp::ShaderModule fragShader(dev, deferred_gbuf_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderer().renderPass(), gPipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}, 0, renderer().samples()};

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
		blendAttachments[1].blendEnable = false;
		blendAttachments[2].blendEnable = false;

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

		gPipe_ = {dev, vkpipe};
	}

	void initLPipe() {
		auto& dev = vulkanDevice();

		// gbuffer input ds
		auto inputBindings = {
			vpp::descriptorBinding( // pos
				vk::DescriptorType::inputAttachment,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding( // normal
				vk::DescriptorType::inputAttachment,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding( // albedo
				vk::DescriptorType::inputAttachment,
				vk::ShaderStageBits::fragment),
		};

		inputDsLayout_ = {dev, inputBindings};
		renderer().inputDsLayout_ = &inputDsLayout_;

		// light ds
		// TODO: statically use shadow data sampler here?
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
		lPipeLayout_ = {dev, {sceneDsLayout_, inputDsLayout_, lightDsLayout_}, {}};

		vpp::ShaderModule vertShader(dev, fullscreen_vert_data);
		vpp::ShaderModule fragShader(dev, deferred_light_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderer().renderPass(), lPipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}, 1, renderer().samples()};

		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		lPipe_ = {dev, vkpipe};
	}

	argagg::parser argParser() const override {
		auto parser = App::argParser();
		parser.definitions.push_back({
			"model", {"--model"},
			"Path of the gltf model to load (dir must contain scene.gltf)", 1
		});
		parser.definitions.push_back({
			"scale", {"--scale"},
			"Apply scale to whole scene", 1
		});
		return parser;
	}

	bool handleArgs(const argagg::parser_results& result) override {
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

	// custom renderer for additional framebuffer attachments
	std::unique_ptr<doi::Renderer>
	createRenderer(const doi::RendererCreateInfo& ri) override {
		return std::make_unique<GRenderer>(ri);
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);

		// render shadow maps
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gPipeLayout_, 0, {sceneDs_}, {});
		for(auto& light : lights_) {
			light.render(cb, gPipeLayout_, shadowData_, *scene_);
		}
	}

	void render(vk::CommandBuffer cb) override {
		// render gbuffers
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gPipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gPipeLayout_, 0, {sceneDs_}, {});
		scene_->render(cb, gPipeLayout_);

		// render lights
		vk::cmdNextSubpass(cb, vk::SubpassContents::eInline);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, lPipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gPipeLayout_, 1, {renderer().inputDs()}, {});
		for(auto& light : lights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				gPipeLayout_, 2, {light.ds()}, {});
			vk::cmdDraw(cb, 6, 1, 0, 0);
		}
	}

	void update(double dt) override {
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
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		switch(ev.keycode) {
			case ny::Keycode::m: // move light here
				lights_[0].data.pd = camera_.pos;
				updateLights_ = true;
				return true;
			case ny::Keycode::p:
				lights_[0].data.pcf = 1 - lights_[0].data.pcf;
				updateLights_ = true;
				return true;
			default:
				break;
		}

		return false;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			doi::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
			App::scheduleRedraw();
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			rotateView_ = ev.pressed;
			return true;
		}

		return false;
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.update || updateLights_) {
			camera_.update = false;

			auto map = sceneUbo_.memoryMap();
			auto span = map.span();
			doi::write(span, matrix(camera_));
			doi::write(span, camera_.pos);
		}

		if(updateLights_) {
			for(auto& l : lights_) {
				l.updateDevice();
			}
			updateLights_ = false;
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	bool needsDepth() const override {
		return true;
	}

	GRenderer& renderer() const {
		return dynamic_cast<GRenderer&>(App::renderer());
	}

protected:
	vpp::Sampler sampler_;
	vpp::TrDsLayout sceneDsLayout_;
	vpp::TrDsLayout primitiveDsLayout_;
	vpp::TrDsLayout materialDsLayout_;

	vpp::TrDsLayout inputDsLayout_; // input attachments
	vpp::TrDsLayout lightDsLayout_;

	vpp::PipelineLayout gPipeLayout_; // gbuf
	vpp::PipelineLayout lPipeLayout_; // light

	vpp::SubBuffer sceneUbo_;
	vpp::TrDs sceneDs_;
	vpp::Pipeline gPipe_; // gbuf
	vpp::Pipeline lPipe_; // light

	std::unique_ptr<doi::Scene> scene_;
	vpp::ViewableImage dummyTex_;

	std::string modelname_ {};
	float sceneScale_ {1.f};

	float time_ {};
	bool rotateView_ {false}; // mouseLeft down
	doi::Camera camera_ {};
	bool updateLights_ = true;

	// shadow
	vk::Format depthFormat_;
	ShadowData shadowData_;
	std::vector<DirLight> lights_;
};

// GRenderer impl
GRenderer::GRenderer(const doi::RendererCreateInfo& ri) :
		doi::Renderer(ri.present) {
	init(ri);
}

void GRenderer::createRenderPass() {
	// TODO: do we need a dependency for msaa?
	// see stage/render.cpp:createRenderPass

	vk::AttachmentDescription attachments[5] {};
	auto msaa = sampleCount_ != vk::SampleCountBits::e1;

	auto aid = 0u;
	auto depthid = -1;
	auto resolveid = -1;
	auto colorid = -1;

	// swapchain color attachments
	// msaa: we resolve to this
	// otherwise this is directly rendered
	attachments[aid].format = scInfo_.imageFormat;
	attachments[aid].samples = vk::SampleCountBits::e1;
	attachments[aid].storeOp = vk::AttachmentStoreOp::store;
	attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[aid].initialLayout = vk::ImageLayout::undefined;
	attachments[aid].finalLayout = vk::ImageLayout::presentSrcKHR;
	if(msaa) {
		attachments[aid].loadOp = vk::AttachmentLoadOp::dontCare;
		resolveid = aid;
	} else {
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		colorid = aid;
	}
	++aid;

	// depth target
	attachments[aid].format = depthFormat_;
	attachments[aid].samples = sampleCount_;
	attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
	attachments[aid].storeOp = vk::AttachmentStoreOp::dontCare;
	attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::clear;
	attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[aid].initialLayout = vk::ImageLayout::undefined;
	attachments[aid].finalLayout = vk::ImageLayout::depthStencilAttachmentOptimal;

	depthid = aid;
	++aid;

	// optiontal multisampled render target
	if(msaa) {
		// multisample color attachment
		attachments[aid].format = scInfo_.imageFormat;
		attachments[aid].samples = sampleCount_;
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].storeOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].initialLayout = vk::ImageLayout::undefined;
		attachments[aid].finalLayout = vk::ImageLayout::presentSrcKHR;

		colorid = aid;
		++aid;
	}

	// gbuffer targets
	auto gbufid = aid;
	for(auto i = 0u; i < 3; ++i) {
		attachments[aid].format = vk::Format::r8g8b8a8Unorm;
		attachments[aid].samples = sampleCount_;
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].storeOp = vk::AttachmentStoreOp::store;
		attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].initialLayout = vk::ImageLayout::undefined;
		attachments[aid].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		++aid;
	}

	vk::SubpassDescription subpasses[2] {};

	// TODO: better layouts...
	// subpass 0: render geometry into gbuffers
	vk::AttachmentReference gbufs[3];
	gbufs[0].attachment = gbufid + 0;
	gbufs[0].layout = vk::ImageLayout::general;
	gbufs[1].attachment = gbufid + 1;
	gbufs[1].layout = vk::ImageLayout::general;
	gbufs[2].attachment = gbufid + 2;
	gbufs[2].layout = vk::ImageLayout::general;

	vk::AttachmentReference depthReference;
	depthReference.attachment = depthid;
	depthReference.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	subpasses[0].pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpasses[0].colorAttachmentCount = 3;
	subpasses[0].pColorAttachments = gbufs;
	subpasses[0].pDepthStencilAttachment = &depthReference;

	// subpass 1: use gbuffers and lights to render final image
	vk::AttachmentReference colorReference;
	colorReference.attachment = colorid;
	colorReference.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference resolveReference;
	resolveReference.attachment = resolveid;
	resolveReference.layout = vk::ImageLayout::colorAttachmentOptimal;

	subpasses[1].pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpasses[1].colorAttachmentCount = 1;
	subpasses[1].pColorAttachments = &colorReference;
	subpasses[1].inputAttachmentCount = 3;
	subpasses[1].pInputAttachments = gbufs;

	if(sampleCount_ != vk::SampleCountBits::e1) {
		subpasses[1].pResolveAttachments = &resolveReference;
	}

	// dependency between subpasses
	vk::SubpassDependency dependency;
	dependency.dependencyFlags = vk::DependencyBits::byRegion; // TODO: correct?
	dependency.srcSubpass = 0u;
	dependency.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	dependency.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	dependency.dstSubpass = 1u;
	dependency.dstAccessMask = vk::AccessBits::inputAttachmentRead;
	dependency.dstStageMask = vk::PipelineStageBits::fragmentShader;

	// create rp
	vk::RenderPassCreateInfo renderPassInfo;
	renderPassInfo.attachmentCount = aid;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 2;
	renderPassInfo.pSubpasses = subpasses;
	renderPassInfo.dependencyCount = 1;
	renderPassInfo.pDependencies = &dependency;

	renderPass_ = {device(), renderPassInfo};
}

void GRenderer::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	std::vector<vk::ImageView> attachments {vk::ImageView {}};
	auto scPos = 0u; // attachments[scPos]: swapchain image

	// depth
	createDepthTarget(scInfo_.imageExtent);
	attachments.push_back(depthTarget_.vkImageView());

	// msaa
	if(sampleCount_ != vk::SampleCountBits::e1) {
		createMultisampleTarget(scInfo_.imageExtent);
		attachments.push_back(multisampleTarget_.vkImageView());
	}

	// gbufs
	for(auto* pgbuf : {&pos_, &normal_, &albedo_}) {
		auto& gbuf = *pgbuf;
		auto usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::inputAttachment |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo::color(device(),
			{size.width, size.height}, usage, {vk::Format::r8g8b8a8Unorm},
			vk::ImageTiling::optimal, sampleCount_);
		dlg_assert(info);
		gbuf = {device(), *info};
		attachments.push_back(gbuf.vkImageView());
	}

	for(auto& buf : bufs) {
		attachments[scPos] = buf.imageView;
		vk::FramebufferCreateInfo info ({},
			renderPass_,
			attachments.size(),
			attachments.data(),
			size.width,
			size.height,
			1);
		buf.framebuffer = {device(), info};
	}

	// update descriptor set
	if(!inputDs_) {
		if(!inputDsLayout_) {
			return;
		}
		inputDs_ = {device().descriptorAllocator(), *inputDsLayout_};
	}

	vpp::DescriptorSetUpdate dsu(inputDs_);
	dsu.inputAttachment({{{}, pos_.vkImageView(), vk::ImageLayout::general}});
	dsu.inputAttachment({{{}, normal_.vkImageView(), vk::ImageLayout::general}});
	dsu.inputAttachment({{{}, albedo_.vkImageView(), vk::ImageLayout::general}});
}

std::vector<vk::ClearValue> GRenderer::clearValues() {
	auto cv = Renderer::clearValues();

	// gbuffers
	vk::ClearValue c {{0.f, 0.f, 0.f, 0.f}};
	for(auto i = 0u; i < 3; ++i) {
		cv.push_back(c);
	}
	return cv;
}

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({"3D View", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
