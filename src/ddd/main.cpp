#include "skybox.hpp"
#include "light.hpp"

#include <stage/camera.hpp>
#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>
#include <stage/quaternion.hpp>
#include <stage/scene/shape.hpp>
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

#include <optional>
#include <vector>
#include <string>

class ViewApp : public doi::App {
public:
	static constexpr auto maxLightSize = 8u;
	using Vertex = doi::Primitive::Vertex;

public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		// renderer already queried the best supported depth format
		depthFormat_ = renderer().depthFormat();
		camera_.perspective.far = 100.f;

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

		primitiveDsLayout_ = {dev, objectBindings};

		// per material
		// push constant range for material
		materialDsLayout_ = doi::Material::createDsLayout(dev, sampler_);
		vk::PushConstantRange pcr;
		pcr.offset = 0;
		pcr.size = sizeof(float) * 8;
		pcr.stageFlags = vk::ShaderStageBits::fragment;

		// per light
		auto lightBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
		};

		lightDsLayout_ = {dev, lightBindings};

		// pipeline layout consisting of all ds layouts and pcrs
		pipeLayout_ = {dev, {
			sceneDsLayout_,
			materialDsLayout_,
			primitiveDsLayout_,
			lightDsLayout_,
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

		// Load Model
		auto s = sceneScale_;
		auto mat = doi::scaleMat<4, float>({s, s, s});
		auto ri = doi::SceneRenderInfo{materialDsLayout_, primitiveDsLayout_,
			pipeLayout_, dummyTex_.vkImageView()};
		if(!(scene_ = doi::loadGltf(modelname_, dev, mat, ri))) {
			return false;
		}

		// add light primitive
		lightMaterial_.emplace(vulkanDevice(), materialDsLayout_,
			dummyTex_.vkImageView(), nytl::Vec{1.f, 1.f, 0.4f, 1.f});
		auto sphere = doi::Sphere{{}, {0.1f, 0.1f, 0.1f}};
		auto shape = doi::generateUV(sphere);
		// auto cube = doi::Cube{{}, {0.1f, 0.1f, 0.1f}};
		// auto shape = doi::generate(cube);
		lightBall_.emplace(vulkanDevice(), shape, primitiveDsLayout_,
			*lightMaterial_, nytl::identity<4, float>());

		// == ubo and stuff ==
		auto sceneUboSize = sizeof(nytl::Mat4f) // proj matrix
			+ maxLightSize * sizeof(Light) // lights
			+ sizeof(nytl::Vec3f) // viewPos
			+ sizeof(std::uint32_t); // numLights
		sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
		sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		// == example light ==
		shadowData_ = initShadowData(dev, pipeLayout_, depthFormat_);
		light_ = {dev, lightDsLayout_, depthFormat_, shadowData_.rp};
		light_.data.pd = {5.8f, 12.0f, 4.f};
		light_.data.type = Light::Type::point;
		updateLights_ = true;

		// descriptors
		vpp::DescriptorSetUpdate sdsu(sceneDs_);
		sdsu.uniform({{sceneUbo_.buffer(), sceneUbo_.offset(), sceneUbo_.size()}});
		sdsu.imageSampler({{shadowData_.sampler,
			light_.shadowMap(),
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

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);
		light_.render(cb, pipeLayout_, shadowData_, *scene_);
	}

	void render(vk::CommandBuffer cb) override {
		skybox_.render(cb);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {sceneDs_}, {});
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 3, {light_.ds()}, {});
		scene_->render(cb, pipeLayout_);
		lightBall_->render(cb, pipeLayout_);
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
			light_.data.pd.x = 7.0 * std::cos(0.2 * time_);
			light_.data.pd.z = 7.0 * std::sin(0.2 * time_);
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
				light_.data.pd = camera_.pos;
				updateLights_ = true;
				return true;
			case ny::Keycode::l:
				moveLight_ ^= true;
				return true;
			case ny::Keycode::p:
				light_.data.pcf = 1 - light_.data.pcf;
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

			// lights
			{
				auto lspan = span;
				doi::write(lspan, light_.data);
			}

			doi::skip(span, sizeof(Light) * maxLightSize);
			doi::write(span, camera_.pos);
			doi::write(span, std::uint32_t(1)); // 1 light

			skybox_.updateDevice(fixedMatrix(camera_));

			// light ball
			// offset slightly from real light position so the geometry
			// itself gets light
			auto off = light_.data.pd;
			off.y -= 0.1;
			off.x -= 0.1;
			off.z -= 0.1;
			auto mat = doi::translateMat(off);
			lightBall_->matrix = mat;
			lightBall_->updateDevice();
		}

		if(updateLights_) {
			light_.updateDevice();
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

protected:
	vpp::Sampler sampler_;
	vpp::TrDsLayout sceneDsLayout_;
	vpp::TrDsLayout primitiveDsLayout_;
	vpp::TrDsLayout materialDsLayout_;
	vpp::TrDsLayout lightDsLayout_;
	vpp::PipelineLayout pipeLayout_;

	vpp::SubBuffer sceneUbo_;
	vpp::TrDs sceneDs_;
	vpp::Pipeline pipe_;

	vpp::ViewableImage dummyTex_;
	bool moveLight_ {false};

	float time_ {};
	bool rotateView_ {false}; // mouseLeft down

	// std::vector<Light> lights_;
	bool updateLights_ {true};

	std::unique_ptr<doi::Scene> scene_; // no default constructor
	doi::Camera camera_ {};

	// shadow
	vk::Format depthFormat_;
	ShadowData shadowData_;
	DirLight light_;

	Skybox skybox_;

	// args
	std::string modelname_ {};
	float sceneScale_ {1.f};

	// light ball
	std::optional<doi::Material> lightMaterial_;
	std::optional<doi::Primitive> lightBall_;
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({"3D View", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
