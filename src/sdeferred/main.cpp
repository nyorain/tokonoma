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
#include <shaders/deferred.light.frag.h>

#include <cstdlib>
#include <random>

// TODO: split in multiple render passes.
//  currently using undefined behavior (sampling attachments)
// TODO: only use ssao when a model has no occlusion texture?
//   or use both then? but we should probably normalize in that case,
//   otherwise we apply ao twice
// TODO(optimization): we could theoretically just use one shadow map at all.
//   requires splitting the light passes though (rendering with light pipe)
//   so might have disadvantage. so not high prio
// TODO; we could reuse float gbuffers for later hdr rendering
//   not that easy though since normal buffer has snorm format...
//   attachments can be used as color *and* input attachments in a
//   subpass.
// NOTE: investigate/compare deferred lightning? http://gameangst.com/?p=141
// TODO(optimization): more efficient point light shadow cube map
//   rendering: only render those sides that we can actually see...

// TODO: hack (renderpass sampling)
// rework App, Renderer handling
vk::Sampler globalSampler;

class GRenderer : public doi::Renderer {
public:
	vpp::Sampler depthSampler_;

public:
	GRenderer(const doi::RendererCreateInfo& ri);

	// for g buffers
	const auto& inputDs() const { return inputDs_; }
	const auto& inputDsLayout() const { return inputDsLayout_; }

	// for light buffer
	const auto& lightInputDs() const { return lightInputDs_; }
	const auto& lightInputDsLayout() const { return lightInputDsLayout_; }

	const auto& depthImage() const { return depthTarget_.image(); }
	auto depthLevels() const { return depthLevels_; }

protected:
	void createRenderPass() override;
	void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) override;
	std::vector<vk::ClearValue> clearValues() override;
	void createDepthTarget(const vk::Extent2D& size) override;

protected:
	// targets of first pass (+ depth)
	vpp::ViewableImage normal_;
	vpp::ViewableImage albedo_;
	vpp::ViewableImage emission_;
	vpp::TrDsLayout inputDsLayout_;
	vpp::TrDs inputDs_;
	vpp::ImageView depthMipView_;

	// target of second pass, hdr
	vpp::ViewableImage light_;
	vpp::TrDsLayout lightInputDsLayout_;
	vpp::TrDs lightInputDs_;

	unsigned depthLevels_ {};
};

class ViewApp : public doi::App {
public:
	using Vertex = doi::Primitive::Vertex;

public:
	bool init(const doi::AppSettings& settings) override {
		auto cpy = settings;
		cpy.rvgSubpass = std::nullopt; // we don't need rvg/gui
		if(!doi::App::init(cpy)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto hostMem = dev.hostMemoryTypes();

		// renderer already queried the best supported depth format
		camera_.perspective.near = 0.1f;
		camera_.perspective.far = 100.f;

		skybox_.init(dev, "../assets/kloofendal2k.hdr",
			renderer().renderPass(), 3u, renderer().samples());

		// tex sampler
		vk::SamplerCreateInfo sci {};
		sci.addressModeU = vk::SamplerAddressMode::repeat;
		sci.addressModeV = vk::SamplerAddressMode::repeat;
		sci.addressModeW = vk::SamplerAddressMode::repeat;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.minLod = 0.0;
		sci.maxLod = 10u; // TODO?
		sci.mipmapMode = vk::SamplerMipmapMode::linear;

		// TODO: enable anisotropy that even for normals and such?
		// probably doesn't have a huge runtime overhead but not sure
		// if really needed for anything besides color
		if(anisotropy_) {
			sci.anisotropyEnable = true;
			sci.maxAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
		}

		sampler_ = {dev, sci};
		globalSampler = sampler_;

		initGPipe(); // subpass 0
		initLPipe(); // subpass 1
		initPPipe(); // subpass 2

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
			+ sizeof(nytl::Mat4f) // inv proj
			+ sizeof(nytl::Vec3f); // viewPos
		sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
		sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		// example light
		lightMaterial_.emplace(vulkanDevice(), materialDsLayout_,
			dummyTex_.vkImageView(), nytl::Vec{1.f, 1.f, 0.4f, 1.f});

		shadowData_ = doi::initShadowData(dev, renderer().depthFormat(),
			lightDsLayout_, materialDsLayout_, primitiveDsLayout_,
			doi::Material::pcr());
#if 0
		{
			auto& l = dirLights_.emplace_back(dev, lightDsLayout_, primitiveDsLayout_,
				shadowData_, camera_.pos, *lightMaterial_);
			l.data.dir = {-3.8f, -9.2f, -5.2f};
			l.data.color = {5.f, 4.f, 2.f};
			l.updateDevice(camera_.pos);
		}
#else
		{
			auto& l = pointLights_.emplace_back(dev, lightDsLayout_, primitiveDsLayout_,
				shadowData_, *lightMaterial_);
			l.data.position = {-1.8f, 6.0f, -2.f};
			l.data.color = {5.f, 4.f, 2.f};
			l.updateDevice();
		}
#endif

		// TODO: hack
		// renderer().depthSampler_ = shadowData_.sampler;

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
		auto pcr = doi::Material::pcr();

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

		gPipe_ = {dev, vkpipe};
	}

	void initLPipe() {
		auto& dev = vulkanDevice();

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

		auto ssaoBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		ssaoDsLayout_ = {dev, ssaoBindings};

		// light pipe
		vk::PushConstantRange pcr {};
		pcr.size = 4;
		pcr.stageFlags = vk::ShaderStageBits::fragment;

		auto& ids = renderer().inputDsLayout();
		lPipeLayout_ = {dev, {sceneDsLayout_, ids, lightDsLayout_,
			ssaoDsLayout_}, {pcr}};

		// TODO: don't use fullscreen here. Only render the areas of the screen
		// that are effected by the light (huge, important optimization
		// when there are many lights in the scene!)
		vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
		vpp::ShaderModule fragShader(dev, deferred_light_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderer().renderPass(), lPipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}, 1u};

		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

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

		gpi.blend.attachmentCount = 1u;
		gpi.blend.pAttachments = &blendAttachment;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		lPipe_ = {dev, vkpipe};

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
	}

	// TODO: use push constant range or ubo to select
	// different tonemapping algorithms
	void initPPipe() {
		auto& dev = vulkanDevice();

		// input ds
		pp_.pipeLayout = {dev, {renderer().lightInputDsLayout()}, {}};

		vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
		vpp::ShaderModule fragShader(dev, deferred_pp_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderer().renderPass(), pp_.pipeLayout, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}, 2};

		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		pp_.pipe = {dev, vkpipe};
	}

	// enable anisotropy if possible
	bool features(vk::PhysicalDeviceFeatures& enable,
			const vk::PhysicalDeviceFeatures& supported) override {
		if(supported.samplerAnisotropy) {
			anisotropy_ = true;
			enable.samplerAnisotropy = true;
		}

		return true;
	}

	argagg::parser argParser() const override {
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
		for(auto& light : pointLights_) {
			light.render(cb, shadowData_, *scene_);
		}
		for(auto& light : dirLights_) {
			light.render(cb, shadowData_, *scene_);
		}
	}

	void render(vk::CommandBuffer cb) override {
		// render gbuffers
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gPipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gPipeLayout_, 0, {sceneDs_}, {});
		scene_->render(cb, gPipeLayout_);

		// NOTE: ideally, don't render these in gbuffer pass...
		/*
		for(auto& l : pointLights_) {
			l.lightBall().render(cb, gPipeLayout_);
		}
		for(auto& l : dirLights_) {
			l.lightBall().render(cb, gPipeLayout_);
		}
		*/

		// create depth mipmaps
		// TODO: not allowed during renderpass, as are our
		// image accesses overall...
		// auto& depthImage = renderer().depthImage();
		// auto depthLevels = renderer().depthLevels();
		// vpp::changeLayout(cb, depthImage,
		// 	vk::ImageLayout::undefined, vk::PipelineStageBits::topOfPipe, {},
		// 	vk::ImageLayout::transferDstOptimal, vk::PipelineStageBits::transfer,
		// 	vk::AccessBits::transferWrite,
		// 	{vk::ImageAspectBits::color, 0, depthLevels, 0, 1});

		// render lights
		// TODO: bad pipe/ds layout, ssaoDs always the same but
		// rebinding it
		vk::cmdNextSubpass(cb, vk::SubpassContents::eInline);
		vk::cmdPushConstants(cb, lPipeLayout_, vk::ShaderStageBits::fragment,
			0, 4, &show_);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, lPipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			lPipeLayout_, 0, {sceneDs_, renderer().inputDs()}, {});
		for(auto& light : pointLights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				lPipeLayout_, 2, {light.ds(), ssaoDs_}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		}
		for(auto& light : dirLights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				lPipeLayout_, 2, {light.ds(), ssaoDs_}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		}

		// post process
		vk::cmdNextSubpass(cb, vk::SubpassContents::eInline);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pp_.pipeLayout, 0, {renderer().lightInputDs()}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen

		// NOTE: skybox isn't tonemapped or anything, remember that
		// screen space stuff; skybox
		vk::cmdNextSubpass(cb, vk::SubpassContents::eInline);
		skybox_.render(cb);
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

		// TODO: this is only here for fps testing
		App::scheduleRedraw();
	}

	bool key(const ny::KeyEvent& ev) override {
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
		if(camera_.update) {
			camera_.update = false;
			auto map = sceneUbo_.memoryMap();
			auto span = map.span();
			auto mat = matrix(camera_);
			doi::write(span, mat);
			doi::write(span, nytl::Mat4f(nytl::inverse(mat)));
			doi::write(span, camera_.pos);

			skybox_.updateDevice(fixedMatrix(camera_));

			// depend on camera position
			updateLights_ = true;
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
	vpp::TrDsLayout lightDsLayout_;
	vpp::TrDsLayout ssaoDsLayout_;

	// shadow
	doi::ShadowData shadowData_;
	std::vector<doi::DirLight> dirLights_;
	std::vector<doi::PointLight> pointLights_;

	vpp::PipelineLayout gPipeLayout_; // gbuf
	vpp::PipelineLayout lPipeLayout_; // light

	vpp::SubBuffer sceneUbo_;
	vpp::TrDs sceneDs_;
	vpp::Pipeline gPipe_; // gbuf
	vpp::Pipeline lPipe_; // light

	static const unsigned SSAO_SAMPLE_COUNT = 32u;
	vpp::TrDs ssaoDs_;
	vpp::ViewableImage ssaoNoise_;
	vpp::SubBuffer ssaoSamples_;

	vpp::ViewableImage dummyTex_;
	std::unique_ptr<doi::Scene> scene_;
	std::optional<doi::Material> lightMaterial_;

	std::string modelname_ {};
	float sceneScale_ {1.f};
	bool anisotropy_ {};

	std::uint32_t show_ {}; // what to show
	float time_ {};
	bool rotateView_ {false}; // mouseLeft down
	doi::Camera camera_ {};
	bool updateLights_ = true;

	doi::Skybox skybox_;

	// post processing
	struct {
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} pp_;
};

// GRenderer impl
GRenderer::GRenderer(const doi::RendererCreateInfo& ri) :
		doi::Renderer(ri.present) {
	dlg_assert(ri.samples == vk::SampleCountBits::e1);

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
		vpp::descriptorBinding( // depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment),
	};

	inputDsLayout_ = {ri.dev, inputBindings};
	inputDs_ = {device().descriptorAllocator(), inputDsLayout_};

	auto lightInputBindings = {
		vpp::descriptorBinding(
			// vk::DescriptorType::inputAttachment,
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment),
	};

	lightInputDsLayout_ = {ri.dev, lightInputBindings};
	lightInputDs_ = {device().descriptorAllocator(), lightInputDsLayout_};

	init(ri);

	// TODO: duplication with shadow sampler in doi/scene/light.cpp
	// depth sampler
	vk::SamplerCreateInfo sci {};
	sci.addressModeU = vk::SamplerAddressMode::clampToBorder;
	sci.addressModeV = vk::SamplerAddressMode::clampToBorder;
	sci.addressModeW = vk::SamplerAddressMode::clampToBorder;
	sci.borderColor = vk::BorderColor::floatOpaqueWhite;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	// sci.compareEnable = true;
	// sci.compareOp = vk::CompareOp::less;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.minLod = 0.0;
	sci.maxLod = 0.25;
	depthSampler_ = {ri.dev, sci};
}

void GRenderer::createRenderPass() {
	dlg_assert(sampleCount_ == vk::SampleCountBits::e1);

	vk::AttachmentDescription attachments[7] {};

	std::uint32_t aid = 0u;
	std::uint32_t depthid = 0xFFFFFFFFu;
	std::uint32_t colorid = 0xFFFFFFFFu;

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
	attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
	colorid = aid;
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

	// gbuffer targets
	auto gbufid = aid;
	auto addBuf = [&](vk::Format format) {
		attachments[aid].format = format;
		attachments[aid].samples = sampleCount_;
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].storeOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].initialLayout = vk::ImageLayout::undefined;
		attachments[aid].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		++aid;
	};

	addBuf(vk::Format::r16g16b16a16Snorm); // normal, matID, occlusion
	addBuf(vk::Format::r8g8b8a8Srgb); // albedo, roughness
	addBuf(vk::Format::r8g8b8a8Unorm); // emission, metallic

	auto lightid = aid;
	addBuf(vk::Format::r16g16b16a16Sfloat); // light

	std::array<vk::SubpassDescription, 4> subpasses {};

	// TODO: better layouts, don't just use general
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
	// depthReference.layout = vk::ImageLayout::depthStencilAttachmentOptimal;
	depthReference.layout = vk::ImageLayout::general;

	subpasses[0].pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpasses[0].colorAttachmentCount = 3;
	subpasses[0].pColorAttachments = gbufs;
	subpasses[0].pDepthStencilAttachment = &depthReference;

	// subpass 1: use gbuffers and lights to render light image
	vk::AttachmentReference lightReference;
	lightReference.attachment = lightid;
	// lightReference.layout = vk::ImageLayout::colorAttachmentOptimal;
	// TODO: for next (preserve) pass...
	lightReference.layout = vk::ImageLayout::general;

	subpasses[1].pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpasses[1].colorAttachmentCount = 1;
	subpasses[1].pColorAttachments = &lightReference;
	subpasses[1].inputAttachmentCount = 3;
	subpasses[1].pInputAttachments = gbufs;
	// TODO(depth): not sure if allowed at all/what i want
	// we use it as sampled input
	subpasses[1].preserveAttachmentCount = 1;
	subpasses[1].pPreserveAttachments = &depthid;

	// subpass 2: post processing, light -> swapchain
	vk::AttachmentReference colorReference;
	colorReference.attachment = colorid;
	colorReference.layout = vk::ImageLayout::colorAttachmentOptimal;

	// vk::AttachmentReference lightInputReference;
	// lightInputReference.attachment = lightid;
	// lightInputReference.layout = vk::ImageLayout::shaderReadOnlyOptimal;

	subpasses[2].pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpasses[2].colorAttachmentCount = 1;
	subpasses[2].pColorAttachments = &colorReference;

	// NOTE: currently sampling from lightid for light scattering
	// has the limit that scattering is only supported for one
	// light
	// subpasses[2].inputAttachmentCount = 1;
	// subpasses[2].pInputAttachments = &lightInputReference;
	subpasses[2].preserveAttachmentCount = 1;
	subpasses[2].pPreserveAttachments = &lightid;

	// render pass for skybox and screen space stuff
	subpasses[3].pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpasses[3].colorAttachmentCount = 1;
	subpasses[3].pColorAttachments = &colorReference;
	subpasses[3].pDepthStencilAttachment = &depthReference;

	// TODO: byRegion correct?
	// dependency between subpasses
	std::array<vk::SubpassDependency, 4> dependencies;
	dependencies[0].dependencyFlags = vk::DependencyBits::byRegion;
	dependencies[0].srcSubpass = 0u;
	dependencies[0].srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	dependencies[0].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	dependencies[0].dstSubpass = 1u;
	dependencies[0].dstAccessMask = vk::AccessBits::inputAttachmentRead;
	dependencies[0].dstStageMask = vk::PipelineStageBits::fragmentShader;

	dependencies[2].dependencyFlags = {};
	dependencies[2].srcSubpass = 0u;
	dependencies[2].srcAccessMask = vk::AccessBits::depthStencilAttachmentWrite;
	dependencies[2].srcStageMask = vk::PipelineStageBits::allGraphics;
	dependencies[2].dstSubpass = 1u;
	dependencies[2].dstAccessMask = vk::AccessBits::shaderRead;
	dependencies[2].dstStageMask = vk::PipelineStageBits::fragmentShader;

	dependencies[1].dependencyFlags = vk::DependencyBits::byRegion;
	dependencies[1].srcSubpass = 1u;
	dependencies[1].srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	dependencies[1].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	dependencies[1].dstSubpass = 2u;
	dependencies[1].dstAccessMask = vk::AccessBits::shaderRead;
	dependencies[1].dstStageMask = vk::PipelineStageBits::fragmentShader;

	dependencies[3].dependencyFlags = {};
	dependencies[3].srcSubpass = 2u;
	dependencies[3].srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	dependencies[3].srcStageMask = vk::PipelineStageBits::allGraphics;
	dependencies[3].dstSubpass = 3u;
	dependencies[3].dstAccessMask = vk::AccessBits::colorAttachmentWrite;
	dependencies[3].dstStageMask = vk::PipelineStageBits::allGraphics;

	// create rp
	vk::RenderPassCreateInfo renderPassInfo;
	renderPassInfo.attachmentCount = aid;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = subpasses.size();
	renderPassInfo.pSubpasses = subpasses.data();
	renderPassInfo.dependencyCount = dependencies.size();
	renderPassInfo.pDependencies = dependencies.data();

	renderPass_ = {device(), renderPassInfo};
}

void GRenderer::createDepthTarget(const vk::Extent2D& size) {
	auto width = size.width;
	auto height = size.height;
	// auto levels = 1 + std::floor(std::log2(std::max(width, height)));
	auto levels = 1;

	// img
	vk::ImageCreateInfo img;
	img.imageType = vk::ImageType::e2d;
	img.format = depthFormat_;
	img.extent.width = width;
	img.extent.height = height;
	img.extent.depth = 1;
	img.mipLevels = levels;
	img.arrayLayers = 1;
	img.sharingMode = vk::SharingMode::exclusive;
	img.tiling = vk::ImageTiling::optimal;
	img.samples = sampleCount_;
	img.usage = vk::ImageUsageBits::depthStencilAttachment |
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
	depthTarget_ = {device(), img, view};

	view.subresourceRange.levelCount = levels;
	view.image = depthTarget_.image();
	depthMipView_ = {device(), view};
	depthLevels_ = levels;
}

void GRenderer::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	std::vector<vk::ImageView> attachments {vk::ImageView {}};
	auto scPos = 0u; // attachments[scPos]: swapchain image

	// depth
	createDepthTarget(scInfo_.imageExtent);
	attachments.push_back(depthTarget_.vkImageView());

	// gbufs
	auto initBuf = [&](vpp::ViewableImage& img, vk::Format format) {
		auto usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::inputAttachment |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo::color(device(),
			{size.width, size.height}, usage, {format},
			vk::ImageTiling::optimal, sampleCount_);
		dlg_assert(info);
		img = {device(), *info};
		attachments.push_back(img.vkImageView());
	};

	initBuf(normal_, vk::Format::r16g16b16a16Snorm); // normal
	initBuf(albedo_, vk::Format::r8g8b8a8Srgb); // albedo
	initBuf(emission_, vk::Format::r8g8b8a8Unorm); // emission

	initBuf(light_, vk::Format::r16g16b16a16Sfloat); // light

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

	// update descriptor sets
	vpp::DescriptorSetUpdate dsu(inputDs_);
	dsu.inputAttachment({{{}, normal_.vkImageView(), vk::ImageLayout::general}});
	dsu.inputAttachment({{{}, albedo_.vkImageView(), vk::ImageLayout::general}});
	dsu.inputAttachment({{{}, emission_.vkImageView(), vk::ImageLayout::general}});

	// TODO: depthSampler_ hack
	if(depthSampler_) {
		// dsu.imageSampler({{depthSampler_, depthTarget_.vkImageView(),
		// 	vk::ImageLayout::general}});
		dsu.imageSampler({{depthSampler_, depthMipView_,
			vk::ImageLayout::general}});
	}

	if(globalSampler) {
		vpp::DescriptorSetUpdate ldsu(lightInputDs_);
		// ldsu.inputAttachment({{{}, light_.vkImageView(),
		// 	vk::ImageLayout::shaderReadOnlyOptimal}});
		ldsu.imageSampler({{globalSampler, light_.vkImageView(),
			vk::ImageLayout::general}});
		vpp::apply({dsu, ldsu});
	}

}

std::vector<vk::ClearValue> GRenderer::clearValues() {
	auto cv = Renderer::clearValues();

	// gbuffers
	vk::ClearValue c {{0.f, 0.f, 0.f, 0.f}};
	for(auto i = 0u; i < 4; ++i) {
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