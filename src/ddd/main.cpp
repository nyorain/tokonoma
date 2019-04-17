#include "skybox.hpp"
#include "mesh.hpp"
#include "material.hpp"

#include <stage/camera.hpp>
#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>
#include <stage/quaternion.hpp>

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

// TODO:
// - seperate light into own file/class; move shadow implementation there?

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
	using Vertex = Primitive::Vertex;

public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		// TODO: getting position right is hard
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
		materialDsLayout_ = Material::createDsLayout(dev, sampler_);
		vk::PushConstantRange pcr;
		pcr.offset = 0;
		pcr.size = sizeof(float) * 7;
		pcr.stageFlags = vk::ShaderStageBits::fragment;

		// pipeline layout consisting of all ds layouts and pcrs
		pipeLayout_ = {dev, {
			sceneDsLayout_,
			objectDsLayout_,
			materialDsLayout_
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

		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		pipe_ = {dev, vkpipe};

		// Load Model
		// const auto filename = "../assets/gltf/test3.gltf";
		const auto filename = "./scene.gltf";
		if (!loadModel(filename)) {
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

	bool loadModel(nytl::StringParam filename) {
		dlg_info(">> Loading model...");
		namespace gltf = tinygltf;

		gltf::TinyGLTF loader;
		gltf::Model model;
		std::string err;
		std::string warn;
		auto res = loader.LoadASCIIFromFile(&model, &err, &warn, filename.c_str());

		// error, warnings
		auto pos = 0u;
		auto end = warn.npos;
		while((end = warn.find_first_of('\n', pos)) != warn.npos) {
			auto d = warn.data() + pos;
			dlg_warn("  {}", std::string_view{d, end - pos});
		}

		pos = 0u;
		while((end = err.find_first_of('\n', pos + 1)) != err.npos) {
			auto d = err.data() + pos;
			dlg_error("  {}", std::string_view{d, end - pos});
		}

		if(!res) {
			dlg_fatal(">> Failed to parse model");
			return false;
		}

		dlg_info(">> Parsing Succesful...");

		// load materials
		dlg_info("Found {} materials", model.materials.size());
		for(auto material : model.materials) {
			auto m = Material(vulkanDevice(), model, material,
				materialDsLayout_, dummyTex_.imageView());
			materials_.push_back(std::move(m));
		}

		// traverse nodes
		dlg_info("Found {} scenes", model.scenes.size());
		auto& scene = model.scenes[model.defaultScene];

		auto mat = nytl::identity<4, float>();
		for(auto nodeid : scene.nodes) {
			dlg_assert(unsigned(nodeid) < model.nodes.size());
			auto& node = model.nodes[nodeid];
			loadNode(model, node, mat);
		}

		return true;
	}

	void loadNode(const tinygltf::Model& model,
			const tinygltf::Node& node,
			nytl::Mat4f matrix) {
		if(!node.matrix.empty()) {
			nytl::Mat4f mat;
			for(auto r = 0u; r < 4; ++r) {
				for(auto c = 0u; c < 4; ++c) {
					// they are column major
					mat[r][c] = node.matrix[c * 4 + r];
				}
			}

			dlg_info(mat);
			matrix = matrix * mat;
		} else if(!node.scale.empty() || !node.translation.empty() || !node.rotation.empty()) {
			// gltf: matrix computed as T * R * S
			auto mat = nytl::identity<4, float>();
			if(!node.scale.empty()) {
				mat[0][0] = node.scale[0];
				mat[1][1] = node.scale[1];
				mat[2][2] = node.scale[2];
			}

			if(!node.rotation.empty()) {
				doi::Quaternion q;
				q.x = node.rotation[0];
				q.y = node.rotation[1];
				q.z = node.rotation[2];
				q.w = node.rotation[3];

				mat = doi::toMat<4>(q) * mat;
			}

			if(!node.translation.empty()) {
				nytl::Vec3f t;
				t.x = node.translation[0];
				t.y = node.translation[1];
				t.z = node.translation[2];
				mat = doi::translateMat(t) * mat;
			}

			dlg_info(mat);
			matrix = matrix * mat;
		}

		for(auto nodeid : node.children) {
			auto& child = model.nodes[nodeid];
			loadNode(model, child, matrix);
		}


		if(node.mesh != -1) {
			auto& mesh = model.meshes[node.mesh];
			for(auto& primitive : mesh.primitives) {
				Material* mat;
				if(primitive.material < 0) {
					mat = &materials_[0];
				} else {
					auto mid = unsigned(primitive.material);
					dlg_assert(mid <= materials_.size());
					mat = &materials_[mid];
				}
				auto m = Primitive(vulkanDevice(), model, primitive,
					objectDsLayout_, *mat);
				primitives_.push_back(std::move(m));
				primitives_.back().matrix = matrix;
			}

		}
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
		// XXX: using other layouts for now
		// we could theoretically also use same pipe layout
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
		*/
		shadow_.pipeLayout = {dev, {sceneDsLayout_, objectDsLayout_}, {}};

		vpp::ShaderModule vertShader(dev, shadowmap_vert_data);
		vpp::GraphicsPipelineInfo gpi {shadow_.rp, shadow_.pipeLayout, {{
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
			shadow_.pipeLayout, 0, {shadow_.ds}, {});

		for(auto& primitive : primitives_) {
			primitive.render(cb, shadow_.pipeLayout, true);
		}

		vk::cmdEndRenderPass(cb);
	}

	void render(vk::CommandBuffer cb) override {
		skybox_.render(cb);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {sceneDs_}, {});
		for(auto& primitive : primitives_) {
			primitive.render(cb, pipeLayout_, false);
		}
	}

	void update(double dt) override {
		App::update(dt);
		App::redraw();
		time_ += dt;

		// movement
		auto kc = appContext().keyboardContext();
		auto fac = dt;

		auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
		auto right = nytl::normalized(nytl::cross(camera_.dir, yUp));
		auto up = nytl::normalized(nytl::cross(camera_.dir, right));
		if(kc->pressed(ny::Keycode::d)) { // right
			camera_.pos += fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::a)) { // left
			camera_.pos += -fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::w)) {
			camera_.pos += fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::s)) {
			camera_.pos += -fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::q)) { // up
			camera_.pos += -fac * up;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::e)) { // down
			camera_.pos += fac * up;
			camera_.update = true;
		}

		if(moveLight_) {
			lights_[0].pd.x = 10.0 * std::cos(0.2 * time_);
			lights_[0].pd.z = 10.0 * std::sin(0.2 * time_);
			updateLights_ = true;
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

		// update model matrix
		for(auto& m : primitives_) {
			m.updateDevice();
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

	std::vector<Material> materials_;
	std::vector<Primitive> primitives_;
	std::vector<Light> lights_;
	bool updateLights_ {true};

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
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({"3D View", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
