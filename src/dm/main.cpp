// Based upon br but serves as playground for 3D rendering.
// dm: dead moose (or damn messy)

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
#include <tkn/scene/pbr.hpp>
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
#include <shaders/dm.model.frag.h>

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

	static constexpr float near = 0.1f;
	static constexpr float far = 20.f;

	static constexpr auto skyGroundAlbedo = Vec3f{0.7f, 0.8f, 1.f};

	// perspective
	static constexpr float fov = 0.5 * nytl::constants::pi;
	static constexpr auto perspectiveMode =
		tkn::ControlledCamera::PerspectiveMode::normal;

	// ortho
	static constexpr float orthoSize = 20.f;

	// area light
	static constexpr auto alCenter = Vec3f{0.f, 0.f, 0.f};
	static constexpr auto alX = Vec3f{0.2f, 0.0f, 0.f};
	static constexpr auto alY = Vec3f{0.0f, 0.2f, 0.f};

public:
	bool init(nytl::Span<const char*> cargs) override {
		Args args;
		if(!Base::doInit(cargs, args)) {
			return false;
		}

		std::fflush(stdout);
		rvgInit();

		// init cam
		cam_.useControl(tkn::ControlledCamera::ControlType::firstPerson);
		auto pers = cam_.defaultPerspective;
		pers.far = -far;
		pers.near = -near;
		pers.fov = fov;
		pers.mode = perspectiveMode;
		cam_.perspective(pers);

		// init pipeline
		auto& dev = vkDevice();
		auto hostMem = dev.hostMemoryTypes();
		vpp::DeviceMemoryAllocator memStage(dev);
		vpp::BufferAllocator bufStage(memStage);

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		tkn::WorkBatcher wb{dev};
		wb.cb = cb;

		auto brdflutFile = openAsset("brdflut.ktx");
		if(!brdflutFile) {
			dlg_error("Couldn't find brdflut.ktx. Generate it using the pbr program");
			return false;
		}

		auto initBrdfLut = tkn::createTexture(wb, tkn::loadImage(std::move(brdflutFile)));
		auto initLtcLut = tkn::createTexture(wb, tkn::loadImage(openAsset("ltc.ktx")));

		brdfLut_ = tkn::initTexture(initBrdfLut, wb);
		areaLight_.ltcLUT = tkn::initTexture(initLtcLut, wb);

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

		// auto initDummyTex = tkn::createTexture(batch, std::move(p), params);
		// dummyTex_ = tkn::initTexture(initDummyTex, batch);
		dummyTex_ = tkn::buildTexture(dev, std::move(p));

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
		auto cameraBindings = std::array{
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		cameraDsLayout_.init(dev, cameraBindings);

		tkn::SkyboxRenderer::PipeInfo pi;
		pi.renderPass = renderPass();
		pi.samples = samples();
		pi.camDsLayout = cameraDsLayout_;
		pi.sampler = sampler_;
		pi.reverseDepth = false;
		skyboxRenderer_.create(vkDevice(), pi);

		// ao
		auto aoBindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle()),
		};

		aoDsLayout_.init(dev, aoBindings);
		std::fflush(stdout);

		// pipeline layout consisting of all ds layouts and pcrs
		pipeLayout_ = {dev, {{
			cameraDsLayout_.vkHandle(),
			scene_.dsLayout().vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			aoDsLayout_.vkHandle(),
		}}, {}};

		vpp::ShaderModule vertShader(dev, br_model_vert_data);
		vpp::ShaderModule fragShader(dev, dm_model_frag_data);
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

		// TODO: only for experiments
		// if(depthClamp_) {
		// 	gpi.rasterization.depthClampEnable = true;
		// }

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

		// TODO: make use batching as well
		sky_ = {vkDevice(), &skyboxRenderer_.dsLayout(), -dirLight_.data.dir,
			skyGroundAlbedo, turbidity_};
		dirLight_.data.color = tkn::f16Scale * sky_.sunIrradiance();

		auto aoUboSize = sizeof(tkn::SH9<Vec4f>) + 4 * 4u
			+ (sizeof(Vec4f)) * 4;
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
		// adsu.imageSampler({{{}, env_.envMap().imageView(),
		// 	vk::ImageLayout::shaderReadOnlyOptimal}});
		adsu.imageSampler({{{}, sky_.cubemap().imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		adsu.imageSampler({{{}, brdfLut_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		// adsu.imageSampler({{{}, env_.irradiance().imageView(),
		// 	vk::ImageLayout::shaderReadOnlyOptimal}});
		adsu.uniform({{{aoUbo_}}});
		adsu.imageSampler({{{}, areaLight_.ltcLUT.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		adsu.apply();

		// PERF: do this in scene initialization to avoid additional
		// data upload
		auto cube = tkn::Cube{{}, {0.05f, 0.05f, 0.05f}};
		auto shape = tkn::generate(cube);
		cubePrimitiveID_ = scene_.addPrimitive(std::move(shape.positions),
			std::move(shape.normals), std::move(shape.indices));

		auto r = 0.05f;
		auto sphere = tkn::Sphere{{}, {r, r, r}};
		shape = tkn::generateUV(sphere);
		// shape = tkn::generateIco(4);
		// for(auto& pos : shape.positions) {
		// 	pos *= r;
		// }

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

		// area light
		auto areaLightColor = nytl::Vec3f{1.f, 0.5f, 0.5f};
		shape = tkn::generateQuad(alCenter, alX, alY);
		areaLight_.primitiveID = scene_.addPrimitive(std::move(shape.positions),
			std::move(shape.normals), std::move(shape.indices));
		lmat.emissionFac = areaLightColor;
		lmat.albedoFac = {};
		lmat.flags |= tkn::Material::Bit::doubleSided;
		areaLight_.materialID = scene_.addMaterial(lmat);
		areaLight_.instanceID = scene_.addInstance(areaLight_.primitiveID,
			nytl::identity<4, float>(), areaLight_.materialID);

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


		// auto id = 0u;
		// auto& ini = scene_.instances()[id];
		// ini.matrix = tkn::lookAt(tkn::Quaternion {}) * ini.matrix;
		// ini.matrix = tkn::toMat<4>(tkn::Quaternion {}) * ini.matrix;
		// scene_.updatedInstance(id);

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
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		// tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
		// 	dirLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, false); // opaque

		// skybox doesn't make sense for orthographic projection
		if(!cam_.isOrthographic()) {
			// tkn::cmdBindGraphicsDescriptors(cb, env_.pipeLayout(), 0, {envCameraDs_});
			// env_.render(cb);
			tkn::cmdBindGraphicsDescriptors(cb, skyboxRenderer_.pipeLayout(),
				0, {envCameraDs_});

			auto& sky = animateSky_ ?
				steppedSkies_[animateSkyID_].sky :
				sky_;
			skyboxRenderer_.render(cb, sky.ds());
		}

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, blendPipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		// tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
		// 	dirLight_.ds(), aoDs_});
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, true); // transparent/blend

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
		// tkn::checkMovement(camera_, swaDisplay(), dt);
		cam_.update(swaDisplay(), dt);

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
			audio_.d3->updateListener(cam_.position(), cam_.dir(), cam_.up());
		}
#endif // BR_AUDIO

		// we currently always redraw to see consistent fps
		// if(moveLight_ || camera_.update || updateLight_ || updateAOParams_) {
		// 	Base::scheduleRedraw();
		// }

		if(animateSky_) {
			animateSkyDeltaAccum_ += dt;
			if(animateSkyDeltaAccum_ >= animateSkyStep) {
				animateSkyDeltaAccum_ = 0.0;
				animateSkyID_ = (animateSkyID_ + 1) % steppedSkies_.size();

				auto& sky = steppedSkies_[animateSkyID_];
				dirLight_.data.color = tkn::f16Scale * sky.sky.sunIrradiance();
				animateSkyUpdate_ = true;
			}

			auto& sky = steppedSkies_[animateSkyID_];
			auto& next = steppedSkies_[animateSkyID_ + 1];
			float f = animateSkyDeltaAccum_ / animateSkyStep;
			dirLight_.data.dir = -nytl::mix(sky.sunDir, next.sunDir, f);
			updateLight_ = true;
		}

		// area light
		auto& ini = scene_.instances()[areaLight_.instanceID];
		ini.matrix = tkn::translateMat(areaLight_.center) * tkn::toMat<4, float>(areaLight_.orient);
		scene_.updatedInstance(areaLight_.instanceID);

		Base::scheduleRedraw();
	}

	void updateSky() {
		newSky_ = {vkDevice(), &skyboxRenderer_.dsLayout(),
			-dirLight_.data.dir, skyGroundAlbedo, turbidity_};
		dirLight_.data.color = tkn::f16Scale * newSky_->sunIrradiance();
		dlg_info("light color: {}", dirLight_.data.color);
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		auto mods = swa_display_active_keyboard_mods(swaDisplay());
		bool shift = mods & swa_keyboard_mod_shift;
		if(shift) {
			switch(ev.keycode) {
				case swa_key_i:
					areaLight_.center.z += 0.01;
					return true;
				case swa_key_o:
					areaLight_.center.z -= 0.01;
					return true;
				case swa_key_k:
					areaLight_.center.y += 0.01;
					return true;
				case swa_key_j:
					areaLight_.center.y -= 0.01;
					return true;
				case swa_key_h:
					areaLight_.center.x -= 0.01;
					return true;
				case swa_key_l:
					areaLight_.center.x += 0.01;
					return true;
				default:
					break;
			}
		}

		auto rotf = 0.025f;
		auto& alo = areaLight_.orient;
		switch(ev.keycode) {
#ifdef BR_AUDIO
			case swa_key_i: // toggle indirect audio
				if(audio_.d3) audio_.d3->toggleIndirect();
				return true;
			case swa_key_u: // toggle direct audio on source
				if(audio_.source) audio_.source->direct = !audio_.source->direct;
				return true;
			case swa_key_o: // move audio source here
				if(audio_.source) audio_.source->position(cam_.position());
				return true;
#endif // BR_AUDIO
			case swa_key_f:
				swa_window_set_state(swaWindow(), swa_window_state_fullscreen);
				break;
			case swa_key_m: // move light here
				moveLight_ = false;
				dirLight_.data.dir = -cam_.position();
				scene_.instances()[dirLight_.instanceID].matrix = dirLightObjMatrix();
				scene_.updatedInstance(dirLight_.instanceID);
				updateLight_ = true;
				updateSky();

				return true;
			case swa_key_n: // move light here
				moveLight_ = false;
				updateLight_ = true;
				pointLight_.data.position = cam_.position();
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

			case swa_key_j: { // toggle projection mode
				if(cam_.isOrthographic()) {
					auto p = cam_.defaultPerspective;
					p.aspect = float(windowSize().x) / windowSize().y;
					p.mode = perspectiveMode;
					p.near = -near;
					p.far = -far;
					p.fov = fov;
					cam_.perspective(p);
				} else {
					auto o = cam_.defaultOrtho;
					o.aspect = float(windowSize().x) / windowSize().y;
					cam_.orthographic(o);
				}

				scheduleRerecord(); // disable/enable skybox
				return true;
			} case swa_key_k: {
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
			} case swa_key_pageup:
				turbidity_ *= 1.1;
				turbidity_ = std::min(turbidity_, 10.f);
				dlg_info("turb: {}", turbidity_);
				updateSky();
				break;
			case swa_key_pagedown:
				turbidity_ /= 1.1;
				turbidity_ = std::max(turbidity_, 1.f);
				dlg_info("turb: {}", turbidity_);
				updateSky();
				break;
			case swa_key_b:
				animateSky_ ^= true;
				if(animateSky_ && steppedSkies_.empty()) {
					dlg_info(">> generating skies...");

					using nytl::constants::pi;
					auto steps = 500u;
					for(auto i = 0u; i < steps; ++i) {
						auto t = float(2 * pi * float(i) / steps);
						auto dir = Vec3f{0.1f + 0.2f * std::sin(t), std::cos(t), std::sin(t)};

						auto& state = steppedSkies_.emplace_back();
						state.sunDir = dir;
						state.sky = {vkDevice(), &skyboxRenderer_.dsLayout(),
							dir, Vec3f{1.f, 1.f, 1.f}, turbidity_};

						// TODO: hack. Apparently needed to not lose
						// connection on wayland?
						// Clean solution would be to just start
						// a new thread.
						// Base::update(0.001);
					}

					dlg_info(">> done!");
				}
				break;
			case swa_key_x:
				alo = tkn::Quaternion::yxz(0, shift ? rotf : -rotf, 0) * alo;
				break;
			case swa_key_y:
				alo = tkn::Quaternion::yxz(shift ? rotf : -rotf, 0, 0) * alo;
				break;
			case swa_key_z:
				alo = tkn::Quaternion::yxz(0, 0, shift ? rotf : -rotf) * alo;
				break;
			default:
				break;
		}

		return false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		// tkn::mouseMove(camera_, camcon_, swaDisplay(), {ev.dx, ev.dy});
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool mouseWheel(float dx, float dy) override {
		if(Base::mouseWheel(dx, dy)) {
			return true;
		}

		cam_.mouseWheel(dy);
		if(auto os = cam_.orthoSize(); os) {
			*os *= std::pow(1.05, dy);
			cam_.orthoSize(*os);
		}

		return true;
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

	void updateDevice() override {
		if(newSky_) {
			sky_ = std::move(*newSky_);
			newSky_ = {};

			updateAOParams_ = true;
			scheduleRerecord();

			vpp::DescriptorSetUpdate adsu(aoDs_);
			adsu.imageSampler({{{}, sky_.cubemap().imageView(),
				vk::ImageLayout::shaderReadOnlyOptimal}});
		}

		if(animateSkyUpdate_) {
			dlg_assert(animateSkyID_ < steppedSkies_.size());
			animateSkyUpdate_ = false;

			updateAOParams_ = true;
			scheduleRerecord();

			auto& sky = steppedSkies_[animateSkyID_].sky;
			vpp::DescriptorSetUpdate adsu(aoDs_);
			adsu.imageSampler({{{}, sky.cubemap().imageView(),
				vk::ImageLayout::shaderReadOnlyOptimal}});
		}

		// update scene ubo
		if(cam_.needsUpdate) {
			// camera_.update = false;

			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			tkn::write(span, cam_.viewProjectionMatrix());
			tkn::write(span, cam_.position());
			tkn::write(span, near);
			tkn::write(span, far);
			map.flush();

			auto envMap = envCameraUbo_.memoryMap();
			auto envSpan = envMap.span();
			tkn::write(envSpan, cam_.fixedViewProjectionMatrix());
			envMap.flush();

			updateLight_ = true;
			cam_.needsUpdate = false;
		}

		auto semaphore = scene_.updateDevice(cam_.viewProjectionMatrix());
		if(semaphore) {
			addSemaphore(semaphore, vk::PipelineStageBits::allGraphics);
			Base::scheduleRerecord();
		}

		if(updateLight_) {
			if(mode_ & modeDirLight) {
				// dirLight_.updateDevice(cameraVP(), near, far);
				dirLight_.updateDevice(cam_.viewProjectionMatrix(), near, far);
			}
			if(mode_ & modePointLight) {
				pointLight_.updateDevice();
			}

			updateLight_ = false;
		}

		// always update them for area light
		if(true || updateAOParams_) {
			updateAOParams_ = false;
			auto map = aoUbo_.memoryMap();
			auto span = map.span();

			if(animateSky_) {
				tkn::write(span, steppedSkies_[animateSkyID_].sky.skyRadiance());
			} else {
				tkn::write(span, sky_.skyRadiance());
			}

			tkn::write(span, mode_);
			tkn::write(span, aoFac_);
			tkn::write(span, u32(1)); // envLods
			tkn::skip(span, sizeof(float)); // pad

			auto& ini = scene_.instances()[areaLight_.instanceID];
			auto& prim = scene_.primitives()[areaLight_.primitiveID];
			for(auto p : prim.positions) {
				tkn::write(span, Vec4f(tkn::multPos(ini.matrix, p)));
			}

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
		// camera_.update = true;
		cam_.aspect({w, h});
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
	// u32 mode_ {modeIrradiance | modeSpecularIBL};
	u32 mode_ {modePointLight | modeDirLight | modeIrradiance};

	vpp::ViewableImage dummyTex_;
	bool moveLight_ {false};

	bool anisotropy_ {}; // whether device supports anisotropy
	bool multiview_ {}; // whether device supports multiview
	bool depthClamp_ {}; // whether the device supports depth clamping
	bool multiDrawIndirect_ {};

	float time_ {};
	// bool rotateView_ {false}; // mouseLeft down

	tkn::Scene scene_; // no default constructor

	tkn::ControlledCamera cam_;
	// tkn::Camera camera_ {};
	// tkn::FPCamCon camcon_ {};

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

	// tkn::Environment env_;
	tkn::SkyboxRenderer skyboxRenderer_;
	tkn::Sky sky_;
	std::optional<tkn::Sky> newSky_;

	float turbidity_ = 2.f;

	vpp::ViewableImage brdfLut_;

	static constexpr auto animateSkyStep = 0.05f;
	float animateSkyDeltaAccum_ {0.f};
	bool animateSky_ {false};
	bool animateSkyUpdate_ {false};
	unsigned animateSkyID_ {0u};

	struct SkyState {
		tkn::Sky sky;
		nytl::Vec3f sunDir;
	};

	std::vector<SkyState> steppedSkies_;

	// args
	// tkn::TouchCameraController touch_;

	struct {
		vpp::ViewableImage ltcLUT;
		u32 instanceID;
		u32 materialID;
		u32 primitiveID;

		nytl::Vec3f center {};
		tkn::Quaternion orient {};
	} areaLight_;

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
	return tkn::appMain<ViewApp>(argc, argv);
}

