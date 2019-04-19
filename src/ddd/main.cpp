#include "skybox.hpp"

#include <stage/camera.hpp>
#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>
#include <stage/quaternion.hpp>
#include <argagg.hpp>

#include <stage/scene/scene.hpp>
#include <stage/scene/primitive.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>
#include <ny/mouseButton.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>

#include <dlg/dlg.hpp>
#include <nytl/mat.hpp>
#include <nytl/stringParam.hpp>

#include <tinygltf.hpp>

#include <shaders/model.vert.h>
#include <shaders/model.frag.h>
#include <shaders/shadowmap.vert.h>

#include <optional>
#include <vector>
#include <string>

// TODO:
// - seperate light into own file/class; move shadow implementation there?
//   shadow mapping really badly implemented. Lots of artefacts, mixing up
//   point and dir light; no support for point light: shadow cube map
//   shadow mapping also doesn't respect alpha. Should discard if albedo.a == 0

bool has_suffix(const std::string_view& str, const std::string_view& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Matches glsl struct
struct Light {
	enum class Type : std::uint32_t {
		point = 1u,
		dir = 2u,
	};

	nytl::Vec3f pd; // position/direction
	Type type;
	nytl::Vec3f color;
	std::uint32_t pcf {0};
};

class ViewApp : public doi::App {
public:
	static constexpr auto maxLightSize = 8u;
	using Vertex = doi::Primitive::Vertex;

public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		camera_.perspective.far = 200.f;

		// == example light ==
		lights_.emplace_back();
		lights_.back().color = {1.f, 1.f, 1.f};
		lights_.back().pd = {5.8f, 4.0f, 4.f};
		lights_.back().type = Light::Type::point;

		// === Init pipeline ===
		auto& dev = vulkanDevice();
		auto hostMem = dev.hostMemoryTypes();
		skybox_.init(dev, renderer().renderPass(), renderer().samples());

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

		// shadow sampler
		sci.addressModeU = vk::SamplerAddressMode::clampToBorder;
		sci.addressModeV = vk::SamplerAddressMode::clampToBorder;
		sci.addressModeW = vk::SamplerAddressMode::clampToBorder;
		sci.borderColor = vk::BorderColor::floatOpaqueWhite;
		sci.mipmapMode = vk::SamplerMipmapMode::nearest;
		sci.compareEnable = true;
		sci.compareOp = vk::CompareOp::lessOrEqual;
		shadow_.sampler = {dev, sci};

		// dummy texture for materials that don't have a texture
		std::array<std::uint8_t, 4> data{255u, 255u, 255u, 255u};
		auto ptr = reinterpret_cast<std::byte*>(data.data());
		dummyTex_ = doi::loadTexture(dev, {1, 1, 1},
			vk::Format::r8g8b8a8Unorm, {ptr, data.size()});

		// per scense; view + projection matrix, lights
		auto sceneBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment),
		};

		sceneDsLayout_ = {dev, sceneBindings};

		// per object; model matrix and material stuff
		auto objectBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		objectDsLayout_ = {dev, objectBindings};

		// per material
		// push constant range for material
		materialDsLayout_ = doi::Material::createDsLayout(dev, sampler_);
		vk::PushConstantRange pcr;
		pcr.offset = 0;
		pcr.size = sizeof(float) * 8;
		pcr.stageFlags = vk::ShaderStageBits::fragment;

		// pipeline layout consisting of all ds layouts and pcrs
		pipeLayout_ = {dev, {
			sceneDsLayout_,
			materialDsLayout_,
			objectDsLayout_,
		}, {pcr}};

		vk::SpecializationMapEntry maxLightsEntry;
		maxLightsEntry.size = sizeof(std::uint32_t);

		vk::SpecializationInfo fragSpec;
		fragSpec.dataSize = sizeof(std::uint32_t);
		fragSpec.pData = &maxLightSize;
		fragSpec.mapEntryCount = 1;
		fragSpec.pMapEntries = &maxLightsEntry;

		vpp::ShaderModule vertShader(dev, model_vert_data);
		vpp::ShaderModule fragShader(dev, model_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderer().renderPass(), pipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment, &fragSpec},
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

		pipe_ = {dev, vkpipe};

		// TODO: support loading .gltb
		// Load Model
		std::string path = "../assets/gltf/";
		std::string file = "test3.gltf";
		if(!modelname_.empty()) {
			if(has_suffix(modelname_, ".gltf")) {
				auto i = modelname_.find_last_of('/');
				if(i == std::string::npos) {
					path = {};
					file = modelname_;
				} else {
					path = modelname_.substr(0, i + 1);
					file = modelname_.substr(i + 1);
				}
			} else {
				path = modelname_;
				if(path.back() != '/') {
					path.push_back('/');
				}
				file = "scene.gltf";
			}
		}

		if (!loadModel(path, file)) {
			return false;
		}

		// == ubo and stuff ==
		auto sceneUboSize = 2 * sizeof(nytl::Mat4f) // light; proj matrix
			+ maxLightSize * sizeof(Light) // lights
			+ sizeof(nytl::Vec3f) // viewPos
			+ sizeof(std::uint32_t); // numLights
		sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
		sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		initShadowPipe();

		// descriptors
		vpp::DescriptorSetUpdate sdsu(sceneDs_);
		sdsu.uniform({{sceneUbo_.buffer(), sceneUbo_.offset(), sceneUbo_.size()}});
		sdsu.imageSampler({{shadow_.sampler, shadow_.target.vkImageView(),
			vk::ImageLayout::depthStencilReadOnlyOptimal}});
		vpp::apply({sdsu});

		return true;
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

	bool loadModel(nytl::StringParam path, nytl::StringParam scene) {
		dlg_info(">> Loading model...");
		namespace gltf = tinygltf;

		gltf::TinyGLTF loader;
		gltf::Model model;
		std::string err;
		std::string warn;

		auto file = std::string(path);
		file += scene;
		auto res = loader.LoadASCIIFromFile(&model, &err, &warn, file.c_str());

		// error, warnings
		auto pos = 0u;
		auto end = warn.npos;
		while((end = warn.find_first_of('\n', pos)) != warn.npos) {
			auto d = warn.data() + pos;
			dlg_warn("  {}", std::string_view{d, end - pos});
			pos = end + 1;
		}

		pos = 0u;
		while((end = err.find_first_of('\n', pos)) != err.npos) {
			auto d = err.data() + pos;
			dlg_error("  {}", std::string_view{d, end - pos});
			pos = end + 1;
		}

		if(!res) {
			dlg_fatal(">> Failed to parse model");
			return false;
		}

		dlg_info(">> Parsing Succesful...");

		// traverse nodes
		dlg_info("Found {} scenes", model.scenes.size());
		auto& sc = model.scenes[model.defaultScene];

		auto s = sceneScale_;
		auto mat = doi::scaleMat<4, float>({s, s, s});
		auto ri = doi::SceneRenderInfo{materialDsLayout_, objectDsLayout_,
			pipeLayout_, dummyTex_.vkImageView()};
		scene_.emplace(vulkanDevice(), path, model, sc, mat, ri);

		return true;
	}

	void initShadowPipe() {
		auto& dev = vulkanDevice();
		auto hostMem = dev.hostMemoryTypes();

		// renderpass
		vk::AttachmentDescription depth {};
		depth.initialLayout = vk::ImageLayout::undefined;
		depth.finalLayout = vk::ImageLayout::depthStencilReadOnlyOptimal;
		depth.format = shadow_.format;
		depth.loadOp = vk::AttachmentLoadOp::clear;
		depth.storeOp = vk::AttachmentStoreOp::store;
		depth.samples = vk::SampleCountBits::e1;

		vk::AttachmentReference depthRef {};
		depthRef.attachment = 0;
		depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

		vk::SubpassDescription subpass {};
		subpass.pDepthStencilAttachment = &depthRef;

		vk::RenderPassCreateInfo rpi {};
		rpi.attachmentCount = 1;
		rpi.pAttachments = &depth;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &subpass;

		shadow_.rp = {dev, rpi};

		// target
		auto targetUsage = vk::ImageUsageBits::depthStencilAttachment |
			vk::ImageUsageBits::sampled;
		auto targetInfo = vpp::ViewableImageCreateInfo::general(dev,
			shadow_.extent, targetUsage, {shadow_.format},
			vk::ImageAspectBits::depth);
		shadow_.target = {dev, *targetInfo};

		// framebuffer
		vk::FramebufferCreateInfo fbi {};
		fbi.attachmentCount = 1;
		fbi.width = shadow_.extent.width;
		fbi.height = shadow_.extent.height;
		fbi.layers = 1u;
		fbi.pAttachments = &shadow_.target.vkImageView();
		fbi.renderPass = shadow_.rp;
		shadow_.fb = {dev, fbi};

		// layouts
		// we could use another pipe layout for shadows since they
		// don't need most of the descriptors
		/*
		auto bindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex), // view/projection
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex) // model
		};

		shadow_.dsLayout = {dev, bindings};
		shadow_.pipeLayout = {dev, {sceneDsLayout_, objectDsLayout_}, {}};
		*/

		vpp::ShaderModule vertShader(dev, shadowmap_vert_data);
		// vpp::GraphicsPipelineInfo gpi {shadow_.rp, shadow_.pipeLayout, {{
		vpp::GraphicsPipelineInfo gpi {shadow_.rp, pipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
		}}, 0, vk::SampleCountBits::e1};

		constexpr auto stride = sizeof(Vertex);
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex
		};

		vk::VertexInputAttributeDescription attributes[1];
		attributes[0].format = vk::Format::r32g32b32Sfloat; // pos

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 1u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;

		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.rasterization.depthBiasEnable = true;

		auto dynamicStates = {
			vk::DynamicState::depthBias,
			vk::DynamicState::viewport,
			vk::DynamicState::scissor
		};
		gpi.dynamic.pDynamicStates = dynamicStates.begin();
		gpi.dynamic.dynamicStateCount = 3;

		gpi.blend.attachmentCount = 0;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		shadow_.pipeline = {dev, vkpipe};

		// setup light ds and ubo
		auto lightUboSize = sizeof(nytl::Mat4f); // projection, view
		shadow_.ds = {dev.descriptorAllocator(), sceneDsLayout_};
		shadow_.ubo = {dev.bufferAllocator(), lightUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		auto& lubo = shadow_.ubo;
		vpp::DescriptorSetUpdate ldsu(shadow_.ds);
		ldsu.uniform({{lubo.buffer(), lubo.offset(), lubo.size()}});
		vpp::apply({ldsu});

		// fill ubo once
		{
			auto map = shadow_.ubo.memoryMap();
			auto span = map.span();
			doi::write(span, lightMatrix());
		}
	}

	nytl::Mat4f lightMatrix() {
		auto& light = lights_[0];
		auto mat = doi::ortho3Sym(20.f, 20.f, 0.5f, 20.f);
		// auto mat = doi::perspective3RH<float>(0.25 * nytl::constants::pi, 1.f, 0.1, 5.f);
		mat = mat * doi::lookAtRH(light.pd,
			{0.f, 0.f, 0.f}, // always looks at center
			{0.f, 1.f, 0.f});
		return mat;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);

		auto width = shadow_.extent.width;
		auto height = shadow_.extent.height;
		vk::ClearValue clearValue {};
		clearValue.depthStencil = {1.f, 0u};

		// draw shadow map!
		vk::RenderPassBeginInfo rpb; // TODO
		vk::cmdBeginRenderPass(cb, {
			shadow_.rp,
			shadow_.fb,
			{0u, 0u, width, height},
			1,
			&clearValue
		}, {});

		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});
		vk::cmdSetDepthBias(cb, 1.25, 0.f, 1.75);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, shadow_.pipeline);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			// shadow_.pipeLayout, 0, {shadow_.ds}, {});
			pipeLayout_, 0, {shadow_.ds}, {});

		scene_->render(cb, pipeLayout_);
		vk::cmdEndRenderPass(cb);
	}

	void render(vk::CommandBuffer cb) override {
		skybox_.render(cb);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {sceneDs_}, {});
		scene_->render(cb, pipeLayout_);
	}

	void update(double dt) override {
		App::update(dt);
		time_ += dt;

		// movement
		auto kc = appContext().keyboardContext();
		if(kc) {
			doi::checkMovement(camera_, *kc, dt);
		}

		if(moveLight_) {
			lights_[0].pd.x = 10.0 * std::cos(0.2 * time_);
			lights_[0].pd.z = 10.0 * std::sin(0.2 * time_);
			updateLights_ = true;
		}

		if(moveLight_ || camera_.update || updateLights_) {
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
				moveLight_ = false;
				lights_[0].pd = camera_.pos;
				updateLights_ = true;
				return true;
			case ny::Keycode::l:
				moveLight_ ^= true;
				return true;
			case ny::Keycode::p:
				lights_[0].pcf = 1 - lights_[0].pcf;
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
			// updateLights_ set to false below

			auto map = sceneUbo_.memoryMap();
			auto span = map.span();

			doi::write(span, matrix(camera_));
			doi::write(span, lightMatrix());

			{ // lights
				auto lspan = span;
				for(auto& l : lights_) {
					doi::write(lspan, l);
				}
			}

			doi::skip(span, sizeof(Light) * maxLightSize);
			doi::write(span, camera_.pos);
			doi::write(span, std::uint32_t(lights_.size()));

			skybox_.updateDevice(fixedMatrix(camera_));
		}

		if(updateLights_) {
			updateLights_ = false;
			auto map = shadow_.ubo.memoryMap();
			auto span = map.span();
			doi::write(span, lightMatrix());
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

protected:
	vpp::Sampler sampler_;
	vpp::TrDsLayout sceneDsLayout_;
	vpp::TrDsLayout objectDsLayout_;
	vpp::TrDsLayout materialDsLayout_;
	vpp::PipelineLayout pipeLayout_;

	vpp::SubBuffer sceneUbo_;
	vpp::TrDs sceneDs_;
	vpp::Pipeline pipe_;

	vpp::ViewableImage dummyTex_;
	bool moveLight_ {false};

	float time_ {};
	bool rotateView_ {false}; // mouseLeft down

	std::vector<Light> lights_;
	bool updateLights_ {true};

	std::optional<doi::Scene> scene_; // no default constructor
	doi::Camera camera_ {};

	// shadow
	struct {
		// static
		vpp::RenderPass rp;
		vpp::Sampler sampler; // with compareOp (?) glsl: sampler2DShadow
		// vpp::TrDsLayout sceneDsLayout;
		// vpp::TrDsLayout objectDsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipeline;
		vk::Format format = vk::Format::d32Sfloat; // TODO: don't hardcode

		vk::Extent3D extent {2048u, 2048u, 1u};

		// should exist per light
		vpp::ViewableImage target;
		vpp::Framebuffer fb;
		vpp::TrDs ds;
		vpp::SubBuffer ubo; // holding the light view matrix
	} shadow_;

	Skybox skybox_;

	// args
	std::string modelname_ {};
	float sceneScale_ {1.f};
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({"3D View", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
