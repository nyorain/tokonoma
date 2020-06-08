// Simple forward renderer mainly as reference for other rendering
// concepts.

#include <tkn/config.hpp>
#include <tkn/ccam.hpp>
#include <tkn/features.hpp>
#include <tkn/singlePassApp.hpp>
#include <tkn/render.hpp>
#include <tkn/transform.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/util.hpp>
#include <tkn/gltf.hpp>
#include <tkn/quaternion.hpp>
#include <tkn/scene/shape.hpp>
#include <tkn/scene/scene.hpp>
#include <tkn/scene/light.hpp>
#include <tkn/scene/scene.hpp>
#include <tkn/scene/environment.hpp>
#include <argagg.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/init.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/submit.hpp>
#include <vpp/vk.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/shader.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>

#include <dlg/dlg.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <nytl/stringParam.hpp>

#include <tinygltf.hpp>

#include <shaders/br.model.vert.h>
#include <shaders/br.model.frag.h>

#include <optional>
#include <array>
#include <vector>
#include <string>

using namespace tkn::types;

class ViewApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	struct Args : Base::Args {
		std::string model;
		float scale {1.f};
	};

	static constexpr u32 modeDirLight = (1u << 0);
	static constexpr u32 modePointLight = (1u << 1);
	static constexpr u32 modeSpecularIBL = (1u << 2);
	static constexpr u32 modeIrradiance = (1u << 3);

public:
	bool init(nytl::Span<const char*> cargs) override {
		Args args;
		if(!Base::doInit(cargs, args)) {
			return false;
		}

		// init pipeline
		auto& dev = vkDevice();
		auto hostMem = dev.hostMemoryTypes();
		vpp::DeviceMemoryAllocator memStage(dev);
		vpp::BufferAllocator bufStage(memStage);

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		tkn::WorkBatcher wb(dev);
		wb.cb = cb;

		auto brdflutFile = openAsset("brdflut.ktx");
		if(!brdflutFile) {
			dlg_error("Couldn't find brdflut.ktx. Generate it using the pbr program");
			return false;
		}

		auto initBrdfLut = tkn::createTexture(wb, tkn::loadImage(std::move(brdflutFile)));
		brdfLut_ = tkn::initTexture(initBrdfLut, wb);

		// tex sampler
		vk::SamplerCreateInfo sci {};
		sci.addressModeU = vk::SamplerAddressMode::repeat;
		sci.addressModeV = vk::SamplerAddressMode::repeat;
		sci.addressModeW = vk::SamplerAddressMode::repeat;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.mipmapMode = vk::SamplerMipmapMode::linear;
		sci.minLod = 0.0;
		sci.maxLod = 100;
		sampler_ = {dev, sci};

		auto idata = std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u};
		auto span = nytl::as_bytes(nytl::span(idata));
		auto p = tkn::wrapImage({1u, 1u, 1u}, vk::Format::r8g8b8a8Unorm, span);

		auto initDummyTex = tkn::createTexture(wb, std::move(p));
		dummyTex_ = tkn::initTexture(initDummyTex, wb);

		// Load scene
		auto mat = nytl::identity<4, float>();
		auto samplerAnisotropy = 1.f;
		if(anisotropy_) {
			samplerAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
		}
		auto ri = tkn::SceneRenderInfo{
			dummyTex_.vkImageView(),
			samplerAnisotropy, false, multiDrawIndirect_
		};

		auto [omodel, path] = tkn::loadGltf(args.model);
		if(!omodel) {
			return false;
		}

		auto& model = *omodel;
		auto scene = model.defaultScene >= 0 ? model.defaultScene : 0;
		auto& sc = model.scenes[scene];

		auto initScene = vpp::InitObject<tkn::Scene>(scene_, wb, path,
			model, sc, mat, ri);
		initScene.object().rescale(4 * args.scale);
		initScene.init(wb, dummyTex_.vkImageView());

		tkn::initShadowData(shadowData_, dev, depthFormat(),
			scene_.dsLayout(), multiview_, depthClamp_);

		// view + projection matrix
		auto cameraBindings = std::array {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		cameraDsLayout_.init(dev, cameraBindings);

		tkn::Environment::InitData initEnv;
		auto convFile = openAsset("convolution.ktx");
		auto irrFile = openAsset("irradiance.ktx");
		if(!convFile) {
			dlg_error("Coulnd't find convolution.ktx. Use the pbr program to generate it");
			return false;
		}
		if(!irrFile) {
			dlg_error("Coulnd't find irradiance.ktx. Use the pbr program to generate it");
			return false;
		}

		env_.create(initEnv, wb,
			tkn::loadImage(std::move(convFile)),
			tkn::loadImage(std::move(irrFile)), sampler_);
		env_.createPipe(vkDevice(), cameraDsLayout_, renderPass(), 0u, samples());
		env_.init(initEnv, wb);

		// ao
		auto aoBindings = std::array {
			// envMap
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
			// brdfLut
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment,  &sampler_.vkHandle()),
			// irradianceMap
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
			// params
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		aoDsLayout_.init(dev, aoBindings);

		// pipeline layout consisting of all ds layouts and pcrs
		pipeLayout_ = {dev, {{
			cameraDsLayout_.vkHandle(),
			scene_.dsLayout().vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			aoDsLayout_.vkHandle(),
		}}, {}};

		vpp::ShaderModule vertShader(dev, br_model_vert_data);
		vpp::ShaderModule fragShader(dev, br_model_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples()};

		gpi.vertex = tkn::Scene::vertexInfo();
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;

		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
		// gpi.depthStencil.depthCompareOp = vk::CompareOp::greaterOrEqual;

		// needed for gltf doubleSided materials
		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

		vpp::Pipeline ppp(dev, gpi.info());
		pipe_ = std::move(ppp);

		gpi.depthStencil.depthWriteEnable = false;
		blendPipe_ = {dev, gpi.info()};

		// camera
		auto cameraUboSize = sizeof(nytl::Mat4f) // proj matrix
			+ sizeof(nytl::Vec3f) + sizeof(float) * 2; // viewPos, near, far
		cameraDs_ = {dev.descriptorAllocator(), cameraDsLayout_};
		cameraUbo_ = {dev.bufferAllocator(), cameraUboSize,
			vk::BufferUsageBits::uniformBuffer, hostMem};

		envCameraUbo_ = {dev.bufferAllocator(), sizeof(nytl::Mat4f),
			vk::BufferUsageBits::uniformBuffer, hostMem};
		envCameraDs_ = {dev.descriptorAllocator(), cameraDsLayout_};

		// example light
		dirLight_ = {wb, shadowData_};
		dirLight_.data.dir = {5.8f, -12.0f, 4.f};

		pointLight_ = {wb, shadowData_};
		pointLight_.data.position = {0.f, 5.0f, 0.f};
		pointLight_.data.radius = 2.f;
		pointLight_.data.attenuation = {1.f, 0.1f, 0.05f};

		updateLight_ = true;

		// bring lights initially into correct layout and stuff
		dirLight_.render(cb, shadowData_, scene_);
		pointLight_.render(cb, shadowData_, scene_);

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		auto aoUboSize = 3 * 4u;
		aoUbo_ = {dev.bufferAllocator(), aoUboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
		aoDs_ = {dev.descriptorAllocator(), aoDsLayout_};

		// descriptors
		vpp::DescriptorSetUpdate sdsu(cameraDs_);
		sdsu.uniform({{{cameraUbo_}}});
		sdsu.apply();

		vpp::DescriptorSetUpdate edsu(envCameraDs_);
		edsu.uniform({{{envCameraUbo_}}});
		edsu.apply();

		vpp::DescriptorSetUpdate adsu(aoDs_);
		adsu.imageSampler({{{}, env_.envMap().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		adsu.imageSampler({{{}, brdfLut_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		adsu.imageSampler({{{}, env_.irradiance().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		adsu.uniform({{{aoUbo_}}});
		adsu.apply();

		// PERF: do this in scene initialization to avoid additional
		// data upload
		auto cube = tkn::Cube{{}, {0.05f, 0.05f, 0.05f}};
		auto shape = tkn::generate(cube);
		cubePrimitiveID_ = scene_.addPrimitive(std::move(shape.positions),
			std::move(shape.normals), std::move(shape.indices));

		auto r = 0.05f;
		// auto sphere = tkn::Sphere{{}, {r, r, r}};
		// shape = tkn::generateUV(sphere);
		shape = tkn::generateIco(4);
		for(auto& pos : shape.positions) {
			pos *= r;
		}

		spherePrimitiveID_ = scene_.addPrimitive(std::move(shape.positions),
			std::move(shape.normals), std::move(shape.indices));

		// init light visualizations
		tkn::Material lmat;

		lmat.emissionFac = dirLight_.data.color;
		lmat.albedoFac = Vec4f(dirLight_.data.color);
		lmat.albedoFac[3] = 1.f;
		// HACK: make sure it doesn't write to depth buffer and isn't
		// rendered into shadow map
		lmat.flags |= tkn::Material::Bit::blend;
		dirLight_.materialID = scene_.addMaterial(lmat);
		dirLight_.instanceID = scene_.addInstance(cubePrimitiveID_,
			dirLightObjMatrix(), dirLight_.materialID);

		lmat.emissionFac = pointLight_.data.color;
		lmat.albedoFac = Vec4f(pointLight_.data.color);
		lmat.albedoFac[3] = 1.f;
		pointLight_.materialID = scene_.addMaterial(lmat);
		pointLight_.instanceID = scene_.addInstance(spherePrimitiveID_,
			pointLightObjMatrix(), pointLight_.materialID);

		// auto id = 0u;
		// auto& ini = scene_.instances()[id];
		// ini.matrix = tkn::toMat<4>(tkn::Quaternion {}) * ini.matrix;
		// scene_.updatedInstance(id);

		return true;
	}

	bool features(tkn::Features& enable, const tkn::Features& supported) override {
		if(!Base::features(enable, supported)) {
			return false;
		}

		if(!supported.base.features.shaderSampledImageArrayDynamicIndexing) {
			dlg_fatal("Required feature shaderSampledImageArrayDynamicIndexing "
				"not supported. Need it for texturing scenes");
			return false;
		}

		enable.base.features.shaderSampledImageArrayDynamicIndexing = true;
		if(supported.base.features.samplerAnisotropy) {
			anisotropy_ = true;
			enable.base.features.samplerAnisotropy = true;
		} else {
			dlg_warn("sampler anisotropy not supported");
		}

		if(supported.multiview.multiview) {
			multiview_ = true;
			enable.multiview.multiview = true;
		} else {
			dlg_warn("Multiview not supported");
		}

		if(supported.base.features.depthClamp) {
			depthClamp_ = true;
			enable.base.features.depthClamp = true;
		} else {
			dlg_warn("DepthClamp not supported");
		}

		if(supported.base.features.multiDrawIndirect) {
			multiDrawIndirect_ = true;
			enable.base.features.multiDrawIndirect = true;
		} else {
			dlg_warn("multiDrawIndirect not supported");
		}

		return true;
	}

	argagg::parser argParser() const override {
		auto parser = Base::argParser();
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

	bool handleArgs(const argagg::parser_results& result,
			Base::Args& bout) override {
		if(!Base::handleArgs(result, bout)) {
			return false;
		}

		auto& out = static_cast<Args&>(bout);
		if(result.has_option("model")) {
			out.model = result["model"].as<const char*>();
		}
		if(result.has_option("scale")) {
			out.scale = result["scale"].as<float>();
		}

		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		Base::beforeRender(cb);
		if(mode_ & modeDirLight) {
			dirLight_.render(cb, shadowData_, scene_);
		}
		if(mode_ & modePointLight) {
			pointLight_.render(cb, shadowData_, scene_);
		}
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});

		scene_.render(cb, pipeLayout_, false); // opaque
		tkn::cmdBindGraphicsDescriptors(cb, env_.pipeLayout(), 0, {envCameraDs_});
		env_.render(cb);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, blendPipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, true); // transparent/blend
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();

		time_ += dt;
		cam_.update(swaDisplay(), dt);
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		switch(ev.keycode) {
			case swa_key_f:
				swa_window_set_state(swaWindow(), swa_window_state_fullscreen);
				break;
			case swa_key_m: // move dir light here
				dirLight_.data.dir = -cam_.position();
				scene_.instances()[dirLight_.instanceID].matrix = dirLightObjMatrix();
				scene_.updatedInstance(dirLight_.instanceID);
				updateLight_ = true;
				return true;
			case swa_key_n: // move point light here
				updateLight_ = true;
				pointLight_.data.position = cam_.position();
				scene_.instances()[pointLight_.instanceID].matrix = pointLightObjMatrix();
				scene_.updatedInstance(pointLight_.instanceID);
				return true;
			case swa_key_p:
				dirLight_.data.flags ^= tkn::lightFlagPcf;
				pointLight_.data.flags ^= tkn::lightFlagPcf;
				updateLight_ = true;
				return true;
			case swa_key_up:
				aoFac_ *= 1.1;
				updateAOParams_ = true;
				dlg_info("ao factor: {}", aoFac_);
				return true;
			case swa_key_down:
				aoFac_ /= 1.1;
				updateAOParams_ = true;
				dlg_info("ao factor: {}", aoFac_);
				return true;
			case swa_key_k1:
				mode_ ^= modeDirLight;
				updateLight_ = true;
				updateAOParams_ = true;
				Base::scheduleRerecord();
				dlg_info("dir light: {}", bool(mode_ & modeDirLight));
				return true;
			case swa_key_k2:
				mode_ ^= modePointLight;
				updateLight_ = true;
				updateAOParams_ = true;
				Base::scheduleRerecord();
				dlg_info("point light: {}", bool(mode_ & modePointLight));
				return true;
			case swa_key_k3:
				mode_ ^= modeSpecularIBL;
				dlg_info("specular IBL: {}", bool(mode_ & modeSpecularIBL));
				updateAOParams_ = true;
				return true;
			case swa_key_k4:
				mode_ ^= modeIrradiance;
				updateAOParams_ = true;
				dlg_info("Static AO: {}", bool(mode_ & modeIrradiance));
				return true;
			case swa_key_k: {
				using Ctrl = tkn::ControlledCamera::ControlType;
				auto ctrl = cam_.controlType();
				if(ctrl == Ctrl::arcball) {
					cam_.useSpaceshipControl();
				} else if(ctrl == Ctrl::spaceship) {
					cam_.useFirstPersonControl();
				} else if(ctrl == Ctrl::firstPerson) {
					cam_.useArcballControl();
				} else if(ctrl == Ctrl::none) {
					cam_.disableControl();
				}

				break;
			} default:
				break;
		}

		return false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool mouseWheel(float dx, float dy) override {
		if(Base::mouseWheel(dx, dy)) {
			return true;
		}

		cam_.mouseWheel(dy);
		return true;
	}


	void updateDevice() override {
		// update scene ubo
		if(cam_.needsUpdate) {
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			tkn::write(span, cam_.viewProjectionMatrix());
			tkn::write(span, cam_.position());
			tkn::write(span, -cam_.near());
			tkn::write(span, -cam_.far());
			map.flush();

			auto envMap = envCameraUbo_.memoryMap();
			auto envSpan = envMap.span();
			tkn::write(envSpan, cam_.fixedViewProjectionMatrix());
			envMap.flush();

			updateLight_ = true;
			cam_.needsUpdate = false;
		}

		// auto semaphore = scene_.updateDevice(cameraVP());
		auto semaphore = scene_.updateDevice(cam_.viewProjectionMatrix());
		if(semaphore) {
			addSemaphore(semaphore, vk::PipelineStageBits::allGraphics);
			Base::scheduleRerecord();
		}

		if(updateLight_) {
			if(mode_ & modeDirLight) {
				dirLight_.updateDevice(cam_.viewProjectionMatrix(),
					-cam_.near(), -cam_.far());
			}
			if(mode_ & modePointLight) {
				pointLight_.updateDevice();
			}

			updateLight_ = false;
		}

		if(updateAOParams_) {
			updateAOParams_ = false;
			auto map = aoUbo_.memoryMap();
			auto span = map.span();

			tkn::write(span, mode_);
			tkn::write(span, aoFac_);
			tkn::write(span, u32(env_.convolutionMipmaps()));
			map.flush();
		}
	}

	// only for visualizing the box/sphere
	nytl::Mat4f dirLightObjMatrix() {
		return tkn::translateMat(-dirLight_.data.dir);
	}

	nytl::Mat4f pointLightObjMatrix() {
		return tkn::translateMat(pointLight_.data.position);
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		cam_.aspect({w, h});
	}

	bool needsDepth() const override { return true; }
	const char* name() const override { return "Basic renderer"; }

protected:
	vpp::Sampler sampler_;
	vpp::TrDsLayout cameraDsLayout_;
	vpp::PipelineLayout pipeLayout_;

	vpp::SubBuffer cameraUbo_;
	vpp::TrDs cameraDs_;
	vpp::SubBuffer envCameraUbo_;
	vpp::TrDs envCameraDs_;
	vpp::Pipeline pipe_;
	vpp::Pipeline blendPipe_; // no depth write

	vpp::TrDsLayout aoDsLayout_;
	vpp::TrDs aoDs_;
	vpp::SubBuffer aoUbo_;
	bool updateAOParams_ {true};
	float aoFac_ {0.025f};

	u32 mode_ {modePointLight | modeDirLight | modeIrradiance};

	vpp::ViewableImage dummyTex_;

	bool anisotropy_ {}; // whether device supports anisotropy
	bool multiview_ {}; // whether device supports multiview
	bool depthClamp_ {}; // whether the device supports depth clamping
	bool multiDrawIndirect_ {};

	float time_ {};
	tkn::Scene scene_; // no default constructor

	tkn::ControlledCamera cam_;

	struct DirLight : public tkn::DirLight {
		using tkn::DirLight::DirLight;
		u32 instanceID;
		u32 materialID;
	};

	struct PointLight : public tkn::PointLight {
		using tkn::PointLight::PointLight;
		u32 instanceID;
		u32 materialID;
	};

	u32 cubePrimitiveID_ {};
	u32 spherePrimitiveID_ {};

	// light and shadow
	tkn::ShadowData shadowData_;
	DirLight dirLight_;
	PointLight pointLight_;
	bool updateLight_ {true};
	tkn::Environment env_;

	vpp::ViewableImage brdfLut_;
};


int main(int argc, const char** argv) {
	return tkn::appMain<ViewApp>(argc, argv);
}
