// Simple forward renderer mainly as reference for other rendering
// concepts. Also serves as 3D audio for some reason.
// TODO: severely crippled for android atm. Fix that.
// See the fragment shader

#include <tkn/config.hpp>
#include <tkn/camera2.hpp>
#include <tkn/singlePassApp.hpp>
#include <tkn/render.hpp>
#include <tkn/window.hpp>
#include <tkn/transform.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/gltf.hpp>
#include <tkn/quaternion.hpp>
#include <tkn/scene/shape.hpp>
#include <tkn/scene/scene.hpp>
#include <tkn/scene/light.hpp>
#include <tkn/scene/scene.hpp>
#include <tkn/scene/environment.hpp>
#include <argagg.hpp>

#include <rvg/context.hpp>
#include <rvg/state.hpp>

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
#include <vector>
#include <string>

#ifdef __ANDROID__
#include <android/native_activity.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#endif

#ifdef TKN_WITH_AUDIO3D
#define BR_AUDIO
#include <tkn/audio.hpp>
#include <tkn/audio3D.hpp>
#include <tkn/sound.hpp>
#include <tkn/sampling.hpp>
#endif

using namespace tkn::types;

class ViewApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	struct Args : Base::Args {
		std::string model;
		float scale {1.f};
		bool noaudio {};
	};

	static constexpr float near = 0.05f;
	static constexpr float far = 25.f;
	static constexpr float fov = 0.5 * nytl::constants::pi;

public:
	bool init(nytl::Span<const char*> cargs) override {
		Args args;
		if(!Base::doInit(cargs, args)) {
			return false;
		}

		std::fflush(stdout);
		rvgInit();

		// init pipeline
		auto& dev = vkDevice();
		auto hostMem = dev.hostMemoryTypes();
		vpp::DeviceMemoryAllocator memStage(dev);
		vpp::BufferAllocator bufStage(memStage);

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		alloc_.emplace(dev);
		auto& alloc = *this->alloc_;
		tkn::WorkBatcher batch{dev, cb, {
				alloc.memDevice, alloc.memHost, memStage,
				alloc.bufDevice, alloc.bufHost, bufStage,
				dev.descriptorAllocator(),
			}
		};

		auto brdflutFile = openAsset("brdflut.ktx");
		if(!brdflutFile) {
			dlg_error("Couldn't find brdflut.ktx. Generate it using the pbr program");
			return false;
		}

		vpp::Init<tkn::Texture> initBrdfLut(batch, tkn::read(std::move(brdflutFile)));
		brdfLut_ = initBrdfLut.init(batch);

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
		auto p = tkn::wrap({1u, 1u}, vk::Format::r8g8b8a8Unorm, span);
		tkn::TextureCreateParams params;
		params.format = vk::Format::r8g8b8a8Unorm;
		dummyTex_ = {batch, std::move(p), params};

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

#ifdef __ANDROID__
		dlg_error("not implemented");
		return false;
		// auto& ac = dynamic_cast<ny::AndroidAppContext&>(appContext());
		// auto* mgr = ac.nativeActivity()->assetManager;
		// auto asset = AAssetManager_open(mgr, "model.glb", AASSET_MODE_BUFFER);
//
		// std::size_t len = AAsset_getLength(asset);
		// auto buf = (std::byte*) AAsset_getBuffer(asset);
//
		// auto omodel = tkn::loadGltf({buf, len});
		// auto path = "";
#else
		auto [omodel, path] = tkn::loadGltf(args.model);
#endif

		if(!omodel) {
			return false;
		}

		std::fflush(stdout);

		auto& model = *omodel;
		auto scene = model.defaultScene >= 0 ? model.defaultScene : 0;
		auto& sc = model.scenes[scene];

		auto initScene = vpp::InitObject<tkn::Scene>(scene_, batch, path,
			model, sc, mat, ri);
		initScene.object().rescale(4 * args.scale);
		initScene.init(batch, dummyTex_.vkImageView());

		std::fflush(stdout);

		shadowData_ = tkn::initShadowData(dev, depthFormat(),
			scene_.dsLayout(), multiview_, depthClamp_);

		// view + projection matrix
		auto cameraBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		cameraDsLayout_ = {dev, cameraBindings};
		std::fflush(stdout);

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

		env_.create(initEnv, batch,
			tkn::read(std::move(convFile)),
			tkn::read(std::move(irrFile)), sampler_);
		env_.createPipe(vkDevice(), cameraDsLayout_, renderPass(), 0u, samples());
		env_.init(initEnv, batch);

		// ao
		auto aoBindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		aoDsLayout_ = {dev, aoBindings};
		std::fflush(stdout);

		// pipeline layout consisting of all ds layouts and pcrs
		pipeLayout_ = {dev, {{
			cameraDsLayout_.vkHandle(),
			scene_.dsLayout().vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			// shadowData_.dsLayout.vkHandle(),
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

		// NOTE: see the gltf material.doubleSided property. We can't switch
		// this per material (without requiring two pipelines) so we simply
		// always render backfaces currently and then dynamically cull in the
		// fragment shader. That is required since some models rely on
		// backface culling for effects (e.g. outlines). See model.frag
		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		// gpi.rasterization.frontFace = vk::FrontFace::clockwise;

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
		dirLight_ = {batch, shadowData_};
		dirLight_.data.dir = {5.8f, -12.0f, 4.f};
		dirLight_.data.color = {5.f, 5.f, 5.f};

		pointLight_ = {batch, shadowData_};
		pointLight_.data.position = {0.f, 5.0f, 0.f};
		pointLight_.data.radius = 2.f;
		pointLight_.data.attenuation = {1.f, 0.1f, 0.05f};

		updateLight_ = true;

		// bring lights initially into correct layout and stuff
		dirLight_.render(cb, shadowData_, scene_);
		pointLight_.render(cb, shadowData_, scene_);

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

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// PERF: do this in scene initialization to avoid additional
		// data upload
		auto cube = tkn::Cube{{}, {0.05f, 0.05f, 0.05f}};
		auto shape = tkn::generate(cube);
		cubePrimitiveID_ = scene_.addPrimitive(std::move(shape.positions),
			std::move(shape.normals), std::move(shape.indices));

		auto sphere = tkn::Sphere{{}, {0.05f, 0.05f, 0.05f}};
		shape = tkn::generateUV(sphere);
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

#ifdef BR_AUDIO
		if(!args.noaudio) {
			// audio
			std::vector<IPLVector3> positions;
			std::vector<IPLTriangle> tris;

			for(auto& ini : scene_.instances()) {
				auto& primitive = scene_.primitives()[ini.primitiveID];
				auto& mat = ini.matrix;

				IPLint32 off = positions.size();
				for(auto& p : primitive.positions) {
					auto pos = tkn::multPos(mat, p);
					positions.push_back({pos.x, pos.y, pos.z});
				}

				for(auto i = 0u; i < primitive.indices.size(); i += 3) {
					tris.push_back({
						(IPLint32) primitive.indices[i + 0] + off,
						(IPLint32) primitive.indices[i + 1] + off,
						(IPLint32) primitive.indices[i + 2] + off});
				}
			}

			std::vector<IPLint32> matIDs(tris.size(), 0);

			audio_.player.emplace("uff");
			auto& ap = *audio_.player;
			dlg_assert(ap.channels() == 2);

			audio_.d3.emplace(ap.rate());
			audio_.d3->addStaticMesh(positions, tris, matIDs);
			ap.audio = &*audio_.d3;

			using MP3Source = tkn::AudioSource3D<tkn::StreamedMP3Audio>;
			auto& s2 = ap.create<MP3Source>(*audio_.d3, ap.bufCaches(), ap,
				openAsset("test.mp3"));
			s2.inner().volume(0.25f);
			s2.position({10.f, 10.f, 0.f});
			audio_.source = &s2;

			ap.create<tkn::ConvolutionAudio>(*audio_.d3, ap.bufCaches());
			ap.start();
		}
#endif // BR_AUDIO


		auto id = 0u;
		auto& ini = scene_.instances()[id];
		// ini.matrix = tkn::lookAt(tkn::Quaternion {}) * ini.matrix;
		ini.matrix = tkn::toMat<4>(tkn::Quaternion {}) * ini.matrix;
		scene_.updatedInstance(id);

		// tkn::init(touch_, camera_, rvgContext());
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
#ifdef BR_AUDIO
		parser.definitions.push_back({
			"no-audio", {"--no-audio"},
			"Disable the whole audio part", 0
		});
#endif // BR_AUDIO
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
#ifdef BR_AUDIO
		if(result.has_option("no-audio")) {
			out.noaudio = true;
		}
#endif // BR_AUDIO

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
		// tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
		// 	dirLight_.ds(), pointLight_.ds(), aoDs_});
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, false); // opaque

		tkn::cmdBindGraphicsDescriptors(cb, env_.pipeLayout(), 0, {envCameraDs_});
		env_.render(cb);

		/*
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, blendPipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), aoDs_});
		// tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
		// 	dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, true); // transparent/blend
		*/

		// if(touch_.alt) {
		// 	rvgContext().bindDefaults(cb);
		// 	windowTransform().bind(cb);
		// 	touch_.paint.bind(cb);
		// 	touch_.move.circle.fill(cb);
		// 	touch_.rotate.circle.fill(cb);
		// }
	}

	void update(double dt) override {
		Base::update(dt);
		time_ += dt;

		// movement
		tkn::checkMovement(camera_, swaDisplay(), dt);
		if(moveLight_) {
			if(mode_ & modePointLight) {
				pointLight_.data.position.x = 7.0 * std::cos(0.2 * time_);
				pointLight_.data.position.z = 7.0 * std::sin(0.2 * time_);
				scene_.instances()[pointLight_.instanceID].matrix = pointLightObjMatrix();
				scene_.updatedInstance(pointLight_.instanceID);
			} else if(mode_ & modeDirLight) {
				dirLight_.data.dir.x = 7.0 * std::cos(0.2 * time_);
				dirLight_.data.dir.z = 7.0 * std::sin(0.2 * time_);
				scene_.instances()[dirLight_.instanceID].matrix = dirLightObjMatrix();
				scene_.updatedInstance(dirLight_.instanceID);
			}
			updateLight_ = true;
		}

		// tkn::update(touch_, dt);

#ifdef BR_AUDIO
		if(audio_.d3) {
			// auto zdir = dir(camera_);
			// auto right = nytl::normalized(nytl::cross(zdir, camera_.up));
			// auto up = nytl::normalized(nytl::cross(right, zdir));
			audio_.d3->updateListener(camera_.pos, dir(camera_), up(camera_));
		}
#endif // BR_AUDIO

		// we currently always redraw to see consistent fps
		// if(moveLight_ || camera_.update || updateLight_ || updateAOParams_) {
		// 	Base::scheduleRedraw();
		// }

		Base::scheduleRedraw();
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		switch(ev.keycode) {
#ifdef BR_AUDIO
			case swa_key_i: // toggle indirect audio
				if(audio_.d3) audio_.d3->toggleIndirect();
				return true;
			case swa_key_u: // toggle direct audio on source
				if(audio_.source) audio_.source->direct = !audio_.source->direct;
				return true;
			case swa_key_o: // move audio source here
				if(audio_.source) audio_.source->position(camera_.pos);
				return true;
#endif // BR_AUDIO
			case swa_key_m: // move light here
				moveLight_ = false;
				dirLight_.data.dir = -camera_.pos;
				scene_.instances()[dirLight_.instanceID].matrix = dirLightObjMatrix();
				scene_.updatedInstance(dirLight_.instanceID);
				updateLight_ = true;
				return true;
			case swa_key_n: // move light here
				moveLight_ = false;
				updateLight_ = true;
				pointLight_.data.position = camera_.pos;
				scene_.instances()[pointLight_.instanceID].matrix = pointLightObjMatrix();
				scene_.updatedInstance(pointLight_.instanceID);
				return true;
			case swa_key_l:
				moveLight_ ^= true;
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
			default:
				break;
		}

		return false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		tkn::mouseMove(camera_, camcon_, swaDisplay(), {ev.dx, ev.dy});
	}


	/*
	bool touchBegin(const swa_touch_event& ev) override {
		if(Base::touchBegin(ev)) {
			return true;
		}

		tkn::touchBegin(touch_, ev.id, {float(ev.x), float(ev.y)}, windowSize());
		return true;
	}

	void touchUpdate(const swa_touch_event& ev) override {
		tkn::touchUpdate(touch_, ev.id, {float(ev.x), float(ev.y)});
		Base::scheduleRedraw();
	}

	bool touchEnd(unsigned id) override {
		if(Base::touchEnd(id)) {
			return true;
		}

		tkn::touchEnd(touch_, id);
		Base::scheduleRedraw();
		return true;
	}
	*/

	nytl::Mat4f projectionMatrix() const {
		auto aspect = float(windowSize().x) / windowSize().y;
		// return tkn::perspective3RH(fov, aspect, near, far);
		// return tkn::perspective3Rev(fov, aspect, near, far);
		// return tkn::perspective3RevInf(fov, aspect, near);
		// return tkn::perspective3RH(fov, aspect, near, far);
		return tkn::perspective3NegZ(fov, aspect, near, far);
	}

	nytl::Mat4f cameraVP() const {
		dlg_debug("==================");
		dlg_debug("viewMatrix: {}", viewMatrix(camera_));
		dlg_debug("lookAtNegZ: {}", lookAtNegZ(camera_.rot, camera_.pos));

		return projectionMatrix() * viewMatrix(camera_);
		// return projectionMatrix() * lookAtNegZ(camera_.rot, camera_.pos);
		// return projectionMatrix() * ml(camera_);
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.update) {
			camera_.update = false;

			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			tkn::write(span, cameraVP());
			tkn::write(span, camera_.pos);
			tkn::write(span, near);
			tkn::write(span, far);
			map.flush();

			auto envMap = envCameraUbo_.memoryMap();
			auto envSpan = envMap.span();
			tkn::write(envSpan, projectionMatrix() * fixedViewMatrix(camera_));
			// tkn::write(envSpan, projectionMatrix() * lookAt(camera_.rot));
			// tkn::write(envSpan, projectionMatrix() * lookAtNegZ(camera_.rot, {0.f, 0.f, 0.f}));
			envMap.flush();

			updateLight_ = true;
		}

		auto semaphore = scene_.updateDevice(cameraVP());
		if(semaphore) {
			addSemaphore(semaphore, vk::PipelineStageBits::allGraphics);
			Base::scheduleRerecord();
		}

		if(updateLight_) {
			if(mode_ & modeDirLight) {
				dirLight_.updateDevice(cameraVP(), near, far);
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
		camera_.update = true;
	}

	bool needsDepth() const override { return true; }
	const char* name() const override { return "Basic renderer"; }

protected:
	struct Alloc {
		vpp::DeviceMemoryAllocator memHost;
		vpp::DeviceMemoryAllocator memDevice;

		vpp::BufferAllocator bufHost;
		vpp::BufferAllocator bufDevice;

		Alloc(const vpp::Device& dev) :
			memHost(dev), memDevice(dev),
			bufHost(memHost), bufDevice(memDevice) {}
	};

	std::optional<Alloc> alloc_;

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
	float aoFac_ {0.25f};

	static constexpr u32 modeDirLight = (1u << 0);
	static constexpr u32 modePointLight = (1u << 1);
	static constexpr u32 modeSpecularIBL = (1u << 2);
	static constexpr u32 modeIrradiance = (1u << 3);
	u32 mode_ {modeIrradiance | modeSpecularIBL};

	tkn::Texture dummyTex_;
	bool moveLight_ {false};

	bool anisotropy_ {}; // whether device supports anisotropy
	bool multiview_ {}; // whether device supports multiview
	bool depthClamp_ {}; // whether the device supports depth clamping
	bool multiDrawIndirect_ {};

	float time_ {};
	// bool rotateView_ {false}; // mouseLeft down

	tkn::Scene scene_; // no default constructor

	tkn::Camera camera_ {};
	tkn::FPCamCon camcon_ {};

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
	tkn::Texture brdfLut_;

	// args
	// tkn::TouchCameraController touch_;

#ifdef BR_AUDIO
	class AudioPlayer : public tkn::AudioPlayer {
	public:
		using tkn::AudioPlayer::AudioPlayer;

		tkn::Audio3D* audio {};
		void renderUpdate() override {
			if(audio) {
				audio->update();
			}
		}
	};

	// using ASource = tkn::AudioSource3D<tkn::StreamedVorbisAudio>;
	using ASource = tkn::AudioSource3D<tkn::StreamedMP3Audio>;
	struct {
		ASource* source;
		std::optional<tkn::Audio3D> d3;
		std::optional<AudioPlayer> player;
	} audio_;
#endif // BR_AUDIO
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
