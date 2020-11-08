#include "geomLight.hpp"
#include "bloom.hpp"
#include "ssr.hpp"
#include "ssao.hpp"
#include "ao.hpp"
#include "combine.hpp"
#include "luminance.hpp"
#include "postProcess.hpp"
#include "scatter.hpp"
#include "lens.hpp"
#include "graph.hpp"

#include <tkn/timeWidget.hpp>
#include <tkn/formats.hpp>
#include <tkn/app.hpp>
#include <tkn/f16.hpp>
#include <tkn/render.hpp>
#include <tkn/ccam.hpp>
#include <tkn/types.hpp>
#include <tkn/transform.hpp>
#include <tkn/util.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/gltf.hpp>
#include <tkn/defer.hpp>
#include <tkn/features.hpp>
#include <tkn/image.hpp>
#include <tkn/scene/shape.hpp>
#include <tkn/scene/light.hpp>
#include <tkn/scene/scene.hpp>
#include <tkn/scene/environment.hpp>
#include <argagg.hpp>

#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <vui/textfield.hpp>
#include <vui/colorPicker.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/debug.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/util/file.hpp>
#include <vpp/formats.hpp>
#include <vpp/init.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>
#include <vpp/handles.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/deferred.pointScatter.frag.h>
#include <shaders/deferred.dirScatter.frag.h>
#include <shaders/deferred.debug.frag.h>

#include <cstdlib>
#include <random>

using namespace tkn::types;
using tkn::f16;

class ViewApp : public tkn::App {
public:
	static constexpr auto pointLight = false;

	static constexpr u32 passScattering = (1u << 1u);
	static constexpr u32 passSSR = (1u << 2u);
	static constexpr u32 passBloom = (1u << 3u);
	static constexpr u32 passSSAO = (1u << 4u);
	static constexpr u32 passLuminance = (1u << 5u);
	static constexpr u32 passLens = (1u << 8u);
	static constexpr u32 passHighlight = (1u << 9u);

	static constexpr u32 passAO = (1u << 6u);
	static constexpr u32 passCombine = (1u << 7u);

	static constexpr auto probeSize = vk::Extent2D {2048, 2048};
	static constexpr auto probeFaceSize = probeSize.width * probeSize.height * 8;

public:
	struct Args : public App::Args {
		float scale {1.f};
		float maxAniso {1.f};
		std::string model;
	};

	bool init(const nytl::Span<const char*> args) override;
	void initTimings();
	void initPasses(tkn::WorkBatcher&);
	void screenshot();
	void addPointLight(nytl::Vec3f pos, nytl::Vec3f color, float radius,
		bool shadowMap);

	void initStaticBuffers(const vk::Extent2D&);
	void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) override;
	bool features(tkn::Features& enable, const tkn::Features& supported) override;
	argagg::parser argParser() const override;
	bool handleArgs(const argagg::parser_results& result, App::Args& out) override;
	void record(const RenderBuffer& buffer) override;
	void update(double dt) override;
	bool key(const swa_key_event& ev) override;
	void mouseMove(const swa_mouse_move_event& ev) override;
	bool mouseButton(const swa_mouse_button_event& ev) override;
	void resize(unsigned w, unsigned h) override;
	void updateDevice() override;
	const char* name() const override { return "Deferred Renderer"; }

protected:
	vpp::TrDsLayout cameraDsLayout_;
	tkn::ShadowData shadowData_;

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

	std::vector<DirLight> dirLights_;
	std::vector<PointLight> pointLights_;
	u32 cubePrimitiveID_ {};
	u32 spherePrimitiveID_ {};

	vpp::SubBuffer cameraUbo_;
	vpp::TrDs cameraDs_;
	vpp::SubBuffer envCameraUbo_;
	vpp::TrDs envCameraDs_;

	vpp::Sampler linearSampler_;
	vpp::Sampler nearestSampler_;

	vpp::ViewableImage dummyTex_;
	vpp::ViewableImage dummyCube_;
	tkn::Scene scene_;
	// std::optional<tkn::Material> lightMaterial_;

	bool anisotropy_ {}; // whether device supports anisotropy
	bool multiview_ {}; // whether device supports multiview
	bool depthClamp_ {}; // whether the device supports depth clamping
	bool multiDrawIndirect_ {};

	float time_ {};
	tkn::ControlledCamera camera_ {};
	bool updateLights_ {true};
	bool updateScene_ {true};

	vk::Format depthFormat_ {};
	bool recreateStaticBufs_ {true};

	vpp::ViewableImage brdfLut_;
	tkn::Environment env_;

	u32 debugMode_ {0};
	u32 renderPasses_ {};

	// image view into the depth buffer that accesses all depth levels
	vpp::ImageView depthMipView_;
	unsigned depthMipLevels_ {};

	// TODO: now that tkn has a f16 class we could use a buffer as well.
	// 1x1px host visible image used to copy the selected model id into
	vpp::Image selectionStage_;
	std::optional<nytl::Vec2ui> selectionPos_;
	vpp::CommandBuffer selectionCb_;
	vpp::Semaphore selectionSemaphore_;
	bool selectionPending_ {};

	// contains temporary values from rendering such as the current
	// depth at the center of the screen or overall brightness
	vpp::SubBuffer renderStage_;
	float desiredLuminance_ {0.2};

	// point light selection. 0 if invalid
	unsigned currentID_ {}; // for id giving
	struct {
		unsigned id {0xFFFFFFFFu};
		vui::dat::Folder* folder {};
		vui::Textfield* radiusField {};
		vui::Textfield* a1Field {};
		vui::Textfield* a2Field {};
		std::unique_ptr<vui::Widget> removedFolder {};
	} selected_;
	vui::dat::Panel* vuiPanel_ {};

	tkn::GaussianBlur blur_;

	FrameGraph frameGraph_;
	FrameTarget* frameTargetBloom_ {};

	GeomLightPass geomLight_;
	BloomPass bloom_;
	SSRPass ssr_;
	SSAOPass ssao_;
	AOPass ao_;
	CombinePass combine_;
	LuminancePass luminance_;
	PostProcessPass pp_;
	LightScatterPass scatter_;
	HighLightPass highlight_;
	LensFlare lens_;

	bool useLensDirt_ {true};
	vpp::ViewableImage lensDirt_;

	bool recreatePasses_ {false};
	vpp::ShaderModule fullVertShader_;

	bool hideTimeWidget_ {false};
	tkn::TimeWidget timeWidget_;

	struct {
		vui::dat::Label* luminance;
		vui::dat::Label* exposure;
		vui::dat::Label* focusDepth;
		vui::dat::Label* dofDepth;
	} gui_;

	// for rendering the scene into a cubemap
	// used for screenshots and probes (light/reflection)
	enum class ProbeState {
		invalid = 0,
		initialized = 1,
		pending = 2,
	};

	struct {
		GeomLightPass geomLight;
		ProbeState state {};
		vpp::CommandBuffer cb;
		vpp::SubBuffer retrieve;

		struct Face {
			vpp::TrDs ds;
			vpp::SubBuffer ubo;

			vpp::TrDs envDs;
			vpp::SubBuffer envUbo;
		};

		std::array<Face, 6u> faces;
	} probe_ {};
};

bool ViewApp::init(const nytl::Span<const char*> pargs) {
	Args args;
	if(!tkn::App::doInit(pargs, args)) {
		return false;
	}

	auto& dev = vkDevice();

	// ignore incorrect debug messages
	// debugMessenger().ignore.push_back(
	// 	"UNASSIGNED-CoreValidation-Shader-FeatureNotEnabled");

	// find supported depth format
	depthFormat_ = tkn::findDepthFormat(vkDevice());
	if(depthFormat_ == vk::Format::undefined) {
		dlg_error("No depth format supported");
		return false;
	}

	// initialization setup
	// stage allocators only valid during this function
	vpp::DeviceMemoryAllocator memStage(dev);
	vpp::BufferAllocator bufStage(memStage);

	// command buffer
	auto& qs = dev.queueSubmitter();
	auto qfam = qs.queue().family();
	auto cb = dev.commandAllocator().get(qfam);
	vk::beginCommandBuffer(cb, {});

	// create wb
	tkn::WorkBatcher wb(dev);
	wb.cb = cb;

	// sampler
	vk::SamplerCreateInfo sci;
	sci.addressModeU = vk::SamplerAddressMode::clampToEdge;
	sci.addressModeV = vk::SamplerAddressMode::clampToEdge;
	sci.addressModeW = vk::SamplerAddressMode::clampToEdge;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.mipmapMode = vk::SamplerMipmapMode::linear;
	sci.minLod = 0.0;
	sci.maxLod = 100.0;
	sci.anisotropyEnable = false;
	// scene rendering has its own samplers
	// sci.anisotropyEnable = anisotropy_;
	// sci.maxAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
	linearSampler_ = {dev, sci};
	vpp::nameHandle(linearSampler_, "linearSampler");

	sci.magFilter = vk::Filter::nearest;
	sci.minFilter = vk::Filter::nearest;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	sci.anisotropyEnable = false;
	nearestSampler_ = {dev, sci};
	vpp::nameHandle(nearestSampler_, "nearestSampler");

	fullVertShader_ = {dev, tkn_fullscreen_vert_data};

	blur_.init(dev, linearSampler_);

	// general layouts
	auto stages =  vk::ShaderStageBits::vertex |
		vk::ShaderStageBits::fragment |
		vk::ShaderStageBits::compute;
	auto sceneBindings = std::array {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer, stages)
	};
	cameraDsLayout_.init(dev, sceneBindings);

	// dummy texture for materials that don't have a texture
	// TODO: we could just create the dummy cube and make the dummy
	// texture just a view into one of the dummy cube faces...
	// TODO: those are required below (mainly by lights and by
	//   materials, both should be fixable).
	auto idata = std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u};
	auto span = nytl::as_bytes(nytl::span(idata));
	auto p = tkn::wrapImage({1u, 1u, 1u}, vk::Format::r8g8b8a8Unorm, span);
	tkn::TextureCreateParams params;
	params.format = vk::Format::r8g8b8a8Unorm;
	auto initDummyTex = createTexture(wb, std::move(p), params);
	dummyTex_ = initTexture(initDummyTex, wb);

	vpp::nameHandle(dummyTex_.image(), "dummyTex.image");
	vpp::nameHandle(dummyTex_.imageView(), "dummyTex.view");

	auto dptr = reinterpret_cast<const std::byte*>(idata.data());
	auto faces = {dptr, dptr, dptr, dptr, dptr, dptr};
	params.cubemap = true;
	p = tkn::wrapImage({1u, 1u, 1u}, vk::Format::r8g8b8a8Unorm, 1, 6u, faces, true);
	auto initDummyCube = createTexture(wb, std::move(p), params);
	dummyCube_ = initTexture(initDummyCube, wb);

	vpp::nameHandle(dummyCube_.image(), "dummyCube.image");
	vpp::nameHandle(dummyCube_.imageView(), "dummyCube.view");

	// ubo and stuff
	auto hostMem = dev.hostMemoryTypes();
	auto camUboSize = sizeof(nytl::Mat4f) // proj matrix
		+ sizeof(nytl::Mat4f) // inv proj
		+ sizeof(nytl::Vec3f) // viewPos
		+ 2 * sizeof(float); // near, far plane
	vpp::Init<vpp::TrDs> initCameraDs(wb.alloc.ds, cameraDsLayout_);
	vpp::Init<vpp::SubBuffer> initCameraUbo(wb.alloc.bufHost, camUboSize,
		vk::BufferUsageBits::uniformBuffer, hostMem);

	vpp::Init<vpp::TrDs> initEnvCameraDs(wb.alloc.ds, cameraDsLayout_);
	vpp::Init<vpp::SubBuffer> initEnvCameraUbo(wb.alloc.bufHost,
		sizeof(nytl::Mat4f), vk::BufferUsageBits::uniformBuffer, hostMem);

	vpp::Init<vpp::SubBuffer> initRenderStage(wb.alloc.bufHost,
		5 * sizeof(f16), vk::BufferUsageBits::transferDst, hostMem, 16u);

	// selection data
	vk::ImageCreateInfo imgi;
	imgi.arrayLayers = 1;
	imgi.extent = {1, 1, 1};
	imgi.format = vk::Format::r32g32b32a32Sfloat;
	imgi.imageType = vk::ImageType::e2d;
	imgi.mipLevels = 1;
	imgi.tiling = vk::ImageTiling::linear;
	imgi.usage = vk::ImageUsageBits::transferDst;
	imgi.samples = vk::SampleCountBits::e1;
	vpp::Init<vpp::Image> initSelectionStage(wb.alloc.memHost, imgi,
		dev.hostMemoryTypes());

	selectionCb_ = dev.commandAllocator().get(qfam,
		vk::CommandPoolCreateBits::resetCommandBuffer);
	selectionSemaphore_ = {dev};

	// environment
	tkn::Environment::InitData envData;
	env_.create(envData, wb, "convolution.ktx", "irradiance.ktx", linearSampler_);
	auto initBrdfLut = createTexture(wb, tkn::loadImage("brdflut.ktx"));

	// Load Model
	auto mat = nytl::identity<4, float>();
	auto samplerAnisotropy = 1.f;
	if(anisotropy_) {
		samplerAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
		samplerAnisotropy = std::max(samplerAnisotropy, args.maxAniso);
	}

	auto ri = tkn::SceneRenderInfo{
		// materialDsLayout_,
		// primitiveDsLayout_,
		dummyTex_.vkImageView(),
		samplerAnisotropy,
		false, multiDrawIndirect_
	};

	auto [omodel, path] = tkn::loadGltf(args.model);
	if(!omodel) {
		std::exit(-1);
	}

	auto model = std::move(*omodel);
	dlg_info("Found {} scenes", model.scenes.size());
	auto scene = model.defaultScene >= 0 ? model.defaultScene : 0;
	auto& sc = model.scenes[scene];

	tkn::Scene::InitData initScene;
	scene_.create(initScene, wb, path, *omodel, sc, mat, ri);
	scene_.rescale(4 * args.scale);

	tkn::initShadowData(shadowData_, dev, depthFormat_,
		scene_.dsLayout(), multiview_, depthClamp_);
	auto dirtPath = TKN_BASE_DIR "/assets/lens/LensDirt_60.JPG";
	auto initDirt = createTexture(wb, tkn::loadImage(dirtPath));

	initPasses(wb);

	rvgInit(pp_.renderPass(), 0, vk::SampleCountBits::e1);

	// allocate
	scene_.init(initScene, wb, dummyTex_.imageView());
	selectionStage_ = initSelectionStage.init();
	cameraDs_ = initCameraDs.init();
	cameraUbo_ = initCameraUbo.init();
	envCameraDs_ = initEnvCameraDs.init();
	envCameraUbo_ = initEnvCameraUbo.init();
	env_.init(envData, wb);
	brdfLut_ = initTexture(initBrdfLut, wb);
	renderStage_ = initRenderStage.init();
	lensDirt_ = initTexture(initDirt, wb);

	// cb
	vk::ImageMemoryBarrier barrier;
	barrier.image = selectionStage_;
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = {};
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::bottomOfPipe,
		{}, {}, {}, {{barrier}});

	// add lights
	// TODO: defer creation
	if(pointLight) {
		addPointLight({-1.8f, 6.0f, -2.f}, {4.f, 3.f, 2.f}, 9.f, true);
	} else {
		auto& l = dirLights_.emplace_back(wb, shadowData_);
		l.data.dir = {-3.8f, -9.2f, -5.2f};
		l.data.color = {2.f, 1.7f, 0.8f};
		l.data.color *= 2;
		l.updateDevice(camera_.viewProjectionMatrix(), -camera_.near(),
			-camera_.far());
		// pp_.params.scatterLightColor = 0.05f * l.data.color;
	}

	// submit and wait
	// NOTE: we could do something on the cpu meanwhile
	vk::endCommandBuffer(cb);
	auto id = qs.add(cb);
	qs.wait(id);

	// PERF: do this in scene initialization to avoid additional
	// data upload
	auto cube = tkn::Cube{{}, {0.05f, 0.05f, 0.05f}};
	auto shape = tkn::generate(cube);
	cubePrimitiveID_ = scene_.addPrimitive(std::move(shape.positions),
		std::move(shape.normals), std::move(shape.indices));

	auto sphere = tkn::Sphere{{}, {0.02f, 0.02f, 0.02f}};
	shape = tkn::generateUV(sphere);
	spherePrimitiveID_ = scene_.addPrimitive(std::move(shape.positions),
		std::move(shape.normals), std::move(shape.indices));

	// scene descriptor, used for some pipelines as set 0 for camera
	// matrix and view position
	vpp::DescriptorSetUpdate sdsu(cameraDs_);
	sdsu.uniform(cameraUbo_);
	sdsu.apply();

	vpp::DescriptorSetUpdate edsu(envCameraDs_);
	edsu.uniform(envCameraUbo_);
	edsu.apply();

	currentID_ = scene_.primitives().size();
	// lightMaterial_.emplace(materialDsLayout_,
	// 	dummyTex_.vkImageView(), scene_.defaultSampler(),
	// 	nytl::Vec{0.f, 0.f, 0.0f, 0.f}, 0.f, 0.f, false,
	// 	nytl::Vec{1.f, 0.9f, 0.5f});

	// gui
	auto& gui = this->gui();

	using namespace vui::dat;
	auto pos = nytl::Vec2f {100.f, 0};
	auto& panel = gui.create<vui::dat::Panel>(pos, 300.f);

	auto createNumTextfield = [](auto& at, auto name, auto initial, auto func) {
		auto start = std::to_string(initial);
		if(start.size() > 4) {
			start.resize(4, '\0');
		}
		auto& t = at.template create<vui::dat::Textfield>(name, start).textfield();
		t.onSubmit = [&, f = std::move(func), name](auto& tf) {
			try {
				auto val = std::stof(std::string(tf.utf8()));
				f(val);
			} catch(const std::exception& err) {
				dlg_error("Invalid float for {}: {}", name, tf.utf8());
				return;
			}
		};
		return &t;
	};

	auto createValueTextfield = [createNumTextfield](auto& at, auto name,
			auto& value, bool* set = {}) {
		return createNumTextfield(at, name, value, [&value, set](auto v){
			value = v;
			if(set) {
				*set = true;
			}
		});
	};

	auto& cbs = panel.create<Checkbox>("update scene").checkbox();
	cbs.set(updateScene_);
	cbs.onToggle = [&](auto&) {
		updateScene_ ^= true;
		camera_.needsUpdate = true;
	};

	// == general/post processing folder ==
	auto& pp = panel.create<vui::dat::Folder>("Post Processing");
	// createValueTextfield(pp, "exposure", params_.exposure, flag);
	createValueTextfield(pp, "aoFactor", ao_.params.factor);
	createValueTextfield(pp, "ssaoPow", ao_.params.ssaoPow);
	createValueTextfield(pp, "tonemap", pp_.params.tonemap);
	createValueTextfield(pp, "scatter strength", combine_.params.scatterStrength);
	createValueTextfield(pp, "dof strength", pp_.params.dofStrength);

	auto& lff = pp.create<vui::dat::Folder>("Lens");
	createValueTextfield(lff, "strength", pp_.params.lensStrength);
	createValueTextfield(lff, "bias", highlight_.params.bias);
	createValueTextfield(lff, "scale", highlight_.params.scale);
	createValueTextfield(lff, "numGhosts", lens_.params.numGhosts);
	createValueTextfield(lff, "dispersal", lens_.params.ghostDispersal);
	createValueTextfield(lff, "distortion", lens_.params.distortion);
	createValueTextfield(lff, "halo width", lens_.params.haloWidth);
	createValueTextfield(lff, "blur hsize", lens_.blurHSize, &this->rerecord_);
	createValueTextfield(lff, "blur fac", lens_.blurFac, &this->rerecord_);
	auto& cb0 = lff.create<Checkbox>("dirt").checkbox();
	cb0.set(useLensDirt_);
	cb0.onToggle = [&](auto& cb) {
		useLensDirt_ = cb.checked();
		recreateStaticBufs_ = true;
	};

	lff.open(false);

	auto& cb1 = pp.create<Checkbox>("ssao").checkbox();
	cb1.set(renderPasses_ & passSSAO);
	cb1.onToggle = [&](auto&) {
		renderPasses_ ^= passSSAO;
		ao_.params.flags ^= AOPass::flagSSAO;
		recreatePasses_ = true;
	};

	auto& cb3 = pp.create<Checkbox>("ssr").checkbox();
	cb3.set(renderPasses_ & passSSR);
	cb3.onToggle = [&](auto&) {
		renderPasses_ ^= passSSR;
		combine_.params.flags ^= CombinePass::flagSSR;
		recreatePasses_ = true;
	};

	auto& cb4 = pp.create<Checkbox>("Bloom").checkbox();
	cb4.set(renderPasses_ & passBloom);
	cb4.onToggle = [&](auto&) {
		renderPasses_ ^= passBloom;
		combine_.params.flags ^= CombinePass::flagBloom;
		recreatePasses_ = true;
	};

	auto& cb8 = pp.create<Checkbox>("Scattering").checkbox();
	cb8.set(renderPasses_ & passScattering);
	cb8.onToggle = [&](auto&) {
		renderPasses_ ^= passScattering;
		combine_.params.flags ^= CombinePass::flagScattering;
		recreatePasses_ = true;
	};

	auto& cb9 = pp.create<Checkbox>("DOF").checkbox();
	cb9.set(pp_.params.flags & PostProcessPass::flagDOF);
	cb9.onToggle = [&](auto&) {
		pp_.params.flags ^= PostProcessPass::flagDOF;
		recreatePasses_ = true;
	};

	auto& cb10 = pp.create<Checkbox>("Luminance").checkbox();
	cb10.set(renderPasses_ & passLuminance);
	cb10.onToggle = [&](auto&) {
		renderPasses_ ^= passLuminance;
		recreatePasses_ = true;
	};

	auto& cb5 = pp.create<Checkbox>("FXAA").checkbox();
	cb5.set(pp_.params.flags & pp_.flagFXAA);
	cb5.onToggle = [&](auto&) {
		pp_.params.flags ^= pp_.flagFXAA;
	};

	auto& cb11 = pp.create<Checkbox>("Lens Flare").checkbox();
	cb11.set(renderPasses_ & passLens);
	cb11.onToggle = [&](auto&) {
		renderPasses_ ^= (passLens | passHighlight);
		pp_.params.flags ^= PostProcessPass::flagLens;
		recreatePasses_ = true;
	};

	auto& cb6 = pp.create<Checkbox>("Diffuse IBL").checkbox();
	cb6.set(ao_.params.flags & AOPass::flagDiffuseIBL);
	cb6.onToggle = [&](auto&) {
		ao_.params.flags ^= AOPass::flagDiffuseIBL;
	};

	auto& cb7 = pp.create<Checkbox>("Specular IBL").checkbox();
	cb7.set(ao_.params.flags & AOPass::flagSpecularIBL);
	cb7.onToggle = [&](auto&) {
		ao_.params.flags ^= AOPass::flagSpecularIBL;
	};

	pp.create<vui::dat::Button>("Output graph").onClick = [&]{
		auto str = graphDot(frameGraph_);
		auto data = (const std::byte*) str.data();
		vpp::writeFile("deferred.dot", {data, str.length()}, false);
	};

	// == bloom folder ==
	auto& bf = panel.create<vui::dat::Folder>("Bloom");
	auto& cbbm = bf.create<Checkbox>("mip blurred").checkbox();
	cbbm.set(bloom_.mipBlurred);
	cbbm.onToggle = [&](auto&) {
		bloom_.mipBlurred ^= true;
		recreateStaticBufs_ = true; // recreate, rerecord
	};
	auto& cbbd = bf.create<Checkbox>("decrease").checkbox();
	cbbd.set(combine_.params.flags & CombinePass::flagBloomDecrease);
	cbbd.onToggle = [&](auto&) {
		combine_.params.flags ^= CombinePass::flagBloomDecrease;
	};
	createValueTextfield(bf, "max levels", bloom_.maxLevels, &recreateStaticBufs_);
	createValueTextfield(bf, "threshold", bloom_.params.highPassThreshold, nullptr);
	createValueTextfield(bf, "strength", combine_.params.bloomStrength, nullptr);
	createValueTextfield(bf, "pow", bloom_.params.bloomPow, nullptr);
	createValueTextfield(bf, "norm", bloom_.params.norm, nullptr);
	bf.open(false);

	// == ssr folder ==
	auto& ssrf = panel.create<vui::dat::Folder>("SSR");
	createValueTextfield(ssrf, "match steps", ssr_.params.marchSteps, nullptr);
	createValueTextfield(ssrf, "binary search steps", ssr_.params.binarySearchSteps, nullptr);
	createValueTextfield(ssrf, "start step size", ssr_.params.startStepSize, nullptr);
	createValueTextfield(ssrf, "step factor", ssr_.params.stepFactor, nullptr);
	createValueTextfield(ssrf, "ldepth threshold", ssr_.params.ldepthThreshold, nullptr);
	createValueTextfield(ssrf, "roughness fac pow", ssr_.params.roughnessFacPow, nullptr);
	ssrf.open(false);

	// == luminance folder ==
	auto& lf = panel.create<vui::dat::Folder>("Luminance");
	auto& lcc = lf.create<Checkbox>("compute").checkbox();
	lcc.set(luminance_.compute);
	lcc.onToggle = [&](const vui::Checkbox& c) {
		luminance_.compute = c.checked();
		recreatePasses_ = true;
	};
	createValueTextfield(lf, "groupDimSize", luminance_.mipGroupDimSize,
		&recreatePasses_);
	lf.open(false);

	// == scatter folder ==
	auto& lsf = panel.create<vui::dat::Folder>("Light Scattering");
	auto& ssc = lsf.create<Checkbox>("shadow").checkbox();
	ssc.set(scatter_.params.flags & scatter_.flagShadow);
	ssc.onToggle = [&](const vui::Checkbox&) {
		scatter_.params.flags ^= scatter_.flagShadow;
	};
	createValueTextfield(lsf, "factor", scatter_.params.fac, nullptr);
	createValueTextfield(lsf, "mie", scatter_.params.mie, nullptr);
	lsf.open(false);

	// == debug values ==
	auto& vg = panel.create<vui::dat::Folder>("Debug");
	gui_.luminance = &vg.create<Label>("luminance", "-");
	gui_.exposure = &vg.create<Label>("exposure", "-");
	gui_.focusDepth = &vg.create<Label>("focus depth", "-");
	gui_.dofDepth = &vg.create<Label>("dof Depth", "-");

	// == light selection folder ==
	vuiPanel_ = &panel;
	selected_.folder = &panel.create<Folder>("Light");
	auto& light = *selected_.folder;
	selected_.radiusField = createNumTextfield(light, "Radius",
			0.0, [this](auto val) {
		dlg_assert(selected_.id != 0xFFFFFFFFu);
		pointLights_[selected_.id].data.radius = val;
		updateLights_ = true;
	});
	selected_.a1Field = createNumTextfield(light, "L Attenuation",
			0.0, [this](auto val) {
		dlg_assert(selected_.id != 0xFFFFFFFFu);
		pointLights_[selected_.id].data.attenuation.y = val;
		updateLights_ = true;
	});
	selected_.a2Field = createNumTextfield(light, "Q Attenuation",
			0.0, [this](auto val) {
		dlg_assert(selected_.id != 0xFFFFFFFFu);
		pointLights_[selected_.id].data.attenuation.z = val;
		updateLights_ = true;
	});

	selected_.removedFolder = panel.remove(*selected_.folder);

	// time widget
	timeWidget_ = {rvgContext(), defaultFont()};
	initTimings();

	return true;
}

void ViewApp::initTimings() {
	// show only timings for active passes; show them in the order
	// they are executed in.
	timeWidget_.reset();
	timeWidget_.addTiming("shadows"); // always the first, no dependencies
	for(auto& pass : frameGraph_.order()) {
		dlg_assert(pass.pass->name);
		if(std::strcmp(pass.pass->name, "geomLight") == 0) {
			timeWidget_.addTiming("geometry");
			timeWidget_.addTiming("light");
			if(geomLight_.renderAO()) {
				timeWidget_.addTiming("ao");
			}
			timeWidget_.addTiming("env");
			timeWidget_.addTiming("geom:blend");
		} else if(std::strcmp(pass.pass->name, "copyCenterDepth") == 0) {
			// no-op
		} else {
			timeWidget_.addTiming(pass.pass->name);
		}
	}
	timeWidget_.complete();
}

void ViewApp::initPasses(tkn::WorkBatcher& wb) {
	// first reset all passes, frees memory and destroys useless objects
	// geomLight_ = {};
	// pp_ = {};
	// bloom_ = {};
	// ssr_ = {};
	// combine_ = {};
	// ssao_ = {};
	// luminance_ = {};
	// ao_ = {};
	// scatter_ = {};

	frameGraph_ = {};

	PassCreateInfo passInfo {
		wb,
		depthFormat_, {
			cameraDsLayout_,
			scene_.dsLayout(),
			shadowData_.dsLayout,
		}, {
			linearSampler_,
			nearestSampler_,
		},
		fullVertShader_
	};

	// NOTE: conservative simplification at the moment; can be optimzied
	renderPasses_ = tkn::bit(renderPasses_, passAO,
		bool(renderPasses_ & passSSAO));
	renderPasses_ = tkn::bit(renderPasses_, passCombine,
		bool(renderPasses_ & (passSSR | passBloom | passScattering)));

	dlg_info("ao pass: {}", bool(renderPasses_ & passAO));
	dlg_info("combine pass: {}", bool(renderPasses_ & passCombine));

	auto& passGeomLight = frameGraph_.addPass();
	passGeomLight.name = "geomLight";
	auto& targetNormals = passGeomLight.addOut(syncScopeFlex,
		geomLight_.normalsTarget().vkImage());
	auto& targetAlbedo = passGeomLight.addOut(syncScopeFlex,
		geomLight_.albedoTarget().vkImage());
	auto& targetEmission = passGeomLight.addOut(syncScopeFlex,
		geomLight_.emissionTarget().vkImage());
	auto& targetLDepth = passGeomLight.addOut(syncScopeFlex,
		geomLight_.ldepthTarget().vkImage());
	auto& targetLight = passGeomLight.addOut(syncScopeFlex,
		geomLight_.lightTarget().vkImage());

	passGeomLight.record = [&](const auto& buf) {
		std::vector<const tkn::PointLight*> pl;
		std::vector<const tkn::DirLight*> dl;
		for(auto& p : pointLights_) pl.push_back(&p);
		for(auto& l : dirLights_) dl.push_back(&l);

		geomLight_.record(buf.cb, buf.size, cameraDs_, scene_, pl, dl,
			envCameraDs_, &env_, &timeWidget_);
	};

	vpp::InitObject initPP(pp_, passInfo, swapchainInfo().imageFormat);
	auto& passPP = frameGraph_.addPass();
	passPP.name = "pp";
	passPP.record = [&](const auto& buf) {
		auto cb = buf.cb;
		vk::cmdBeginRenderPass(cb, {
			pp_.renderPass(),
			buf.fb,
			{0u, 0u, buf.size.width, buf.size.height},
			0, nullptr
		}, {});

		pp_.record(cb, debugMode_);
		timeWidget_.addTimestamp(cb);

		// gui stuff
		vpp::DebugLabel debugLabel(vkDevice(), cb, "GUI");
		rvgContext().bindDefaults(cb);
		gui().draw(cb);

		rvgContext().bindDefaults(cb);
		rvgWindowTransform().bind(cb);
		timeWidget_.draw(cb);

		vk::cmdEndRenderPass(cb);
	};

	FrameTarget* targetSSAO {};
	SSAOPass::InitData initSSAO;
	if(renderPasses_ & passSSAO) {
		ssao_.create(initSSAO, passInfo);

		auto& passSSAO = frameGraph_.addPass();
		passSSAO.name = "ssao";
		passSSAO.addIn(targetLDepth, ssao_.dstScopeDepth());
		passSSAO.addIn(targetNormals, ssao_.dstScopeNormals());
		targetSSAO = &passSSAO.addOut(ssao_.srcScopeTarget(),
			ssao_.target().vkImage());

		passSSAO.record = [&](const auto& buf) {
			ssao_.record(buf.cb, buf.size, cameraDs_);
			timeWidget_.addTimestamp(buf.cb);
		};
	}

	FrameTarget* targetAO = &targetLight;
	AOPass::InitData initAO;
	if(renderPasses_ & passAO) {
		ao_.create(initAO, passInfo);

		auto& passAO = frameGraph_.addPass();
		passAO.name = "ao";
		targetAO = &passAO.addInOut(targetLight, ao_.scopeLight());
		passAO.addIn(targetLDepth, ao_.dstScopeGBuf());
		passAO.addIn(targetAlbedo, ao_.dstScopeGBuf());
		passAO.addIn(targetNormals, ao_.dstScopeGBuf());
		passAO.addIn(targetEmission, ao_.dstScopeGBuf());
		if(targetSSAO){
			passAO.addIn(*targetSSAO, ao_.dstScopeSSAO());
		}

		passAO.record = [&](const auto& buf) {
			ao_.record(buf.cb, cameraDs_, buf.size);
			timeWidget_.addTimestamp(buf.cb);
		};
	}

	FrameTarget* targetBloom {};
	BloomPass::InitData initBloom;
	if(renderPasses_ & passBloom) {
		bloom_.create(initBloom, passInfo);

		auto& passBloom = frameGraph_.addPass();
		passBloom.name = "bloom";
		passBloom.addIn(targetEmission, bloom_.dstScopeEmission());
		passBloom.addIn(*targetAO, bloom_.dstScopeLight());
		targetBloom = &passBloom.addOut(bloom_.srcScopeTarget(),
			bloom_.target());
		frameTargetBloom_ = targetBloom;

		passBloom.record = [&](const auto& buf) {
			bloom_.record(buf.cb, geomLight_.emissionTarget().image(), buf.size);
			timeWidget_.addTimestamp(buf.cb);
		};
	}

	FrameTarget* targetSSR {};
	SSRPass::InitData initSSR;
	if(renderPasses_ & passSSR) {
		ssr_.create(initSSR, passInfo);

		auto& passSSR = frameGraph_.addPass();
		passSSR.name = "ssr";
		passSSR.addIn(targetLDepth, ssr_.dstScopeDepth());
		passSSR.addIn(targetNormals, ssr_.dstScopeNormals());
		targetSSR = &passSSR.addOut(ssr_.srcScopeTarget(),
			ssr_.target().vkImage());

		passSSR.record = [&](const auto& buf) {
			ssr_.record(buf.cb, cameraDs_, buf.size);
			timeWidget_.addTimestamp(buf.cb);
		};
	}

	FrameTarget* targetScatter {};
	LightScatterPass::InitData initScatter;
	if(renderPasses_ & passScattering) {
		scatter_.create(initScatter, passInfo, !pointLight);

		auto& passScatter = frameGraph_.addPass();
		passScatter.name = "scatter";
		passScatter.addIn(targetLDepth, scatter_.dstScopeDepth());
		targetScatter = &passScatter.addOut(scatter_.srcScopeTarget(),
			scatter_.target().vkImage());

		passScatter.record = [&](const auto& buf) {
			vk::DescriptorSet lightDs = pointLight ?
				pointLights_[0].ds() :
				dirLights_[0].ds();
			scatter_.record(buf.cb, buf.size, cameraDs_, lightDs);
			timeWidget_.addTimestamp(buf.cb);
		};
	}

	FrameTarget* targetPPInput = targetAO;
	CombinePass::InitData initCombine;
	if(renderPasses_ & passCombine) {
		combine_.create(initCombine, passInfo);

		auto& passCombine = frameGraph_.addPass();
		passCombine.name = "combine";
		passCombine.addIn(targetLDepth, combine_.dstScopeDepth());
		passCombine.addIn(*targetAO, combine_.dstScopeLight());
		if(targetBloom) {
			passCombine.addIn(*targetBloom, combine_.dstScopeBloom());
		}
		if(targetSSR) {
			passCombine.addIn(*targetSSR, combine_.dstScopeSSR());
		}
		if(targetScatter) {
			passCombine.addIn(*targetScatter, combine_.dstScopeScatter());
		}
		auto& targetCombined = passCombine.addInOut(targetEmission,
			combine_.scopeTarget());
		passCombine.record = [&](const auto& buf) {
			combine_.record(buf.cb, buf.size);
			timeWidget_.addTimestamp(buf.cb);
		};

		targetPPInput = &targetCombined;
	}

	FrameTarget* targetLuminance {};
	LuminancePass::InitData initLuminance;
	if(renderPasses_ & passLuminance) {
		luminance_.create(initLuminance, passInfo);

		auto& passLum = frameGraph_.addPass();
		passLum.name = "luminance";
		passLum.addIn(*targetPPInput, luminance_.dstScopeLight());
		targetLuminance = &passLum.addOut(
			luminance_.srcScopeTarget(),
			luminance_.target().vkImage());

		passLum.record = [&](const auto& buf) {
			luminance_.record(buf.cb, buf.size);
			timeWidget_.addTimestamp(buf.cb);
		};
	}

	FrameTarget* targetHighlight {};
	HighLightPass::InitData initHighlight;
	if(renderPasses_ & passHighlight) {
		highlight_.create(initHighlight, passInfo);

		auto& passHighlight = frameGraph_.addPass();
		passHighlight.name = "highlight";
		passHighlight.addIn(targetLight, highlight_.dstScopeLight());
		passHighlight.addIn(targetEmission, highlight_.dstScopeEmission());
		targetHighlight = &passHighlight.addOut(highlight_.srcScopeTarget(),
			highlight_.target().vkImage());

		passHighlight.record = [&](const auto& buf) {
			highlight_.record(buf.cb, buf.size);
			timeWidget_.addTimestamp(buf.cb);
		};
	}

	FrameTarget* targetLens {};
	LensFlare::InitData initLens;
	if(renderPasses_ & passLens) {
		dlg_assert(targetHighlight);
		lens_.create(initLens, passInfo, blur_);

		auto& passLens = frameGraph_.addPass();
		passLens.name = "lens";
		passLens.addIn(*targetHighlight, lens_.dstScopeLight());
		targetLens = &passLens.addOut(lens_.srcScopeTarget(),
			lens_.target().vkImage());

		passLens.record = [&](const auto& buf) {
			lens_.record(buf.cb, blur_, buf.size);
			timeWidget_.addTimestamp(buf.cb);
		};
	}

	passPP.addIn(*targetPPInput, pp_.dstScopeInput());
	if(targetLens) {
		passPP.addIn(*targetLens, pp_.dstScopeInput());
	}

	if(debugMode_ != 0) {
		if(targetSSR) {
			passPP.addIn(*targetSSR, pp_.dstScopeInput());
		}
		if(targetSSAO) {
			passPP.addIn(*targetSSAO, pp_.dstScopeInput());
		}
		if(targetBloom) {
			passPP.addIn(*targetBloom, pp_.dstScopeInput());
		}
		if(targetLuminance) {
			passPP.addIn(*targetLuminance, pp_.dstScopeInput());
		}
		if(targetScatter) {
			passPP.addIn(*targetScatter, pp_.dstScopeInput());
		}

		passPP.addIn(targetLDepth, pp_.dstScopeInput());
		passPP.addIn(targetAlbedo, pp_.dstScopeInput());
		passPP.addIn(targetNormals, pp_.dstScopeInput());
	} else if(pp_.params.flags & pp_.flagDOF) {
		passPP.addIn(targetLDepth, pp_.dstScopeInput());

		// pseudo-pass that copies the center pixel of the depth target
		// so we know what depth is currently focused and can adopt dof
		auto& passCopyDepth = frameGraph_.addPass();
		passCopyDepth.name = "copyCenterDepth"; // special in initTimings
		passCopyDepth.addIn(targetLDepth, {
			vk::PipelineStageBits::transfer,
			vk::ImageLayout::transferSrcOptimal,
			vk::AccessBits::transferRead,
		});
		passCopyDepth.record = [&](const auto& buf) {
			vk::BufferImageCopy copy;
			copy.bufferOffset = renderStage_.offset();
			copy.imageExtent = {1, 1, 1};
			copy.imageOffset = {i32(buf.size.width / 2u), i32(buf.size.height / 2u), 0};
			copy.imageSubresource.aspectMask = vk::ImageAspectBits::color;
			copy.imageSubresource.layerCount = 1;
			copy.imageSubresource.mipLevel = 0;
			vk::cmdCopyImageToBuffer(buf.cb, geomLight_.ldepthTarget().image(),
				vk::ImageLayout::transferSrcOptimal,
				renderStage_.buffer(), {{copy}});
		};
	}

	dlg_assert(frameGraph_.check());
	frameGraph_.compute();

	// targets
	auto dstAlbedo = targetAlbedo.producer.scope;
	auto dstNormals = targetNormals.producer.scope;
	auto dstEmission = targetEmission.producer.scope;
	auto dstLDepth = targetLDepth.producer.scope;
	auto dstLight = targetLight.producer.scope;
	auto dstDepth = SyncScope {};
	vpp::InitObject initGeomLight(geomLight_, passInfo,
		dstNormals, dstAlbedo, dstEmission, dstDepth, dstLDepth, dstLight,
		!(renderPasses_ & passAO));

	if(timeWidget_.valid()) {
		initTimings();
	}

	// finish initialization
	initGeomLight.init();
	initPP.init(passInfo);

	if(renderPasses_ & passBloom) {
		bloom_.init(initBloom, passInfo);
	}
	if(renderPasses_ & passSSR) {
		ssr_.init(initSSR, passInfo);
	}
	if(renderPasses_ & passSSAO) {
		ssao_.init(initSSAO, passInfo);
	}
	if(renderPasses_ & passLuminance) {
		luminance_.init(initLuminance, passInfo);
	}
	if(renderPasses_ & passAO) {
		ao_.init(initAO, passInfo);
	}
	if(renderPasses_ & passCombine) {
		combine_.init(initCombine, passInfo);
	}
	if(renderPasses_ & passScattering) {
		scatter_.init(initScatter, passInfo);
	}
	if(renderPasses_ & passHighlight) {
		highlight_.init(initHighlight, passInfo);
	}
	if(renderPasses_ & passLens) {
		lens_.init(initLens, passInfo, blur_);
	}

	// others
	env_.createPipe(vkDevice(), cameraDsLayout_, geomLight_.renderPass(), 1,
		vk::SampleCountBits::e1);
}

static constexpr auto defaultAttachmentUsage =
	vk::ImageUsageBits::colorAttachment |
	vk::ImageUsageBits::inputAttachment |
	vk::ImageUsageBits::transferDst |
	vk::ImageUsageBits::transferSrc |
	vk::ImageUsageBits::sampled;

void ViewApp::initStaticBuffers(const vk::Extent2D& size) {
	auto& dev = vkDevice();
	std::vector<vk::ImageView> attachments;

	vpp::DeviceMemoryAllocator memStage(dev);
	vpp::BufferAllocator bufStage(memStage);

	// TODO: no cb needed here for now, future passes might use
	// it though so it should be supported
	tkn::WorkBatcher wb(dev);

	GeomLightPass::InitBufferData geomLightData;
	geomLight_.createBuffers(geomLightData, wb, size, depthFormat_);

	BloomPass::InitBufferData bloomData;
	if(renderPasses_ & passBloom) {
		bloom_.createBuffers(bloomData, wb, size);
	}

	SSRPass::InitBufferData ssrData;
	if(renderPasses_ & passSSR) {
		ssr_.createBuffers(ssrData, wb, size);
	}

	SSAOPass::InitBufferData ssaoData;
	if(renderPasses_ & passSSAO) {
		ssao_.createBuffers(ssaoData, wb, size);
	}

	LuminancePass::InitBufferData lumData;
	if(renderPasses_ & passLuminance) {
		luminance_.createBuffers(lumData, wb, size);
	}

	LightScatterPass::InitBufferData scatterData;
	if(renderPasses_ & passScattering) {
		scatter_.createBuffers(scatterData, wb, size);
	}

	HighLightPass::InitBufferData highlightData;
	if(renderPasses_ & passHighlight) {
		highlight_.createBuffers(highlightData, wb, size);
	}

	LensFlare::InitBufferData lensData;
	if(renderPasses_ & passLens) {
		lens_.createBuffers(lensData, wb, size);
	}

	// init
	geomLight_.initBuffers(geomLightData, size,
		env_.irradiance().imageView(),
		env_.envMap().imageView(),
		env_.convolutionMipmaps(),
		brdfLut_.imageView());
	auto ppInput = geomLight_.lightTarget().vkImageView();

	auto bloomView = dummyTex_.vkImageView();
	if(renderPasses_ & passBloom) {
		bloom_.initBuffers(bloomData, geomLight_.lightTarget().imageView());
		bloomView = bloom_.fullView();
		frameTargetBloom_->subres.levelCount = bloom_.levelCount();
	}
	auto ssrView = dummyTex_.vkImageView();
	if(renderPasses_ & passSSR) {
		ssr_.initBuffers(ssrData, geomLight_.ldepthTarget().imageView(),
			geomLight_.normalsTarget().imageView());
		ssrView = ssr_.targetView();
	}
	auto ssaoView = dummyTex_.vkImageView();
	if(renderPasses_ & passSSAO) {
		ssao_.initBuffers(ssaoData, geomLight_.ldepthTarget().imageView(),
			geomLight_.normalsTarget().imageView(), size);
		ssaoView = ssao_.targetView();
	}

	auto scatterView = dummyTex_.vkImageView();
	if(renderPasses_ & passScattering) {
		scatter_.initBuffers(scatterData, size,
			geomLight_.ldepthTarget().imageView());
		scatterView = scatter_.target().imageView();
	}

	auto highlightView = dummyTex_.vkImageView();
	if(renderPasses_ & passHighlight) {
		highlight_.initBuffers(highlightData,
			geomLight_.lightTarget().imageView(),
			geomLight_.emissionTarget().imageView());
		highlightView = highlight_.target().imageView();
	}

	auto lensView = dummyTex_.vkImageView();
	if(renderPasses_ & passLens) {
		lens_.initBuffers(lensData, highlightView, blur_);
		lensView = lens_.target().imageView();
	}

	if(renderPasses_ & passAO) {
		ao_.updateInputs(
			geomLight_.lightTarget().imageView(),
			geomLight_.albedoTarget().imageView(),
			geomLight_.emissionTarget().imageView(),
			geomLight_.ldepthTarget().imageView(),
			geomLight_.normalsTarget().imageView(),
			ssaoView,
			env_.irradiance().imageView(),
			env_.envMap().imageView(),
			env_.convolutionMipmaps(),
			brdfLut_.imageView());
	}
	if(renderPasses_ & passCombine) {
		ppInput = geomLight_.emissionTarget().vkImageView();
		combine_.updateInputs(
			geomLight_.emissionTarget().imageView(),
			geomLight_.lightTarget().imageView(),
			geomLight_.ldepthTarget().imageView(),
			bloomView, ssrView, scatterView);
	}

	auto lumView = dummyTex_.vkImageView();
	if(renderPasses_ & passLuminance) {
		luminance_.initBuffers(lumData, ppInput, size);
		lumView = luminance_.target().imageView();
	}

	auto lensDirt = useLensDirt_ ?
		lensDirt_.vkImageView() :
		dummyTex_.vkImageView();
	auto shadowView = pointLight ?
		pointLights_[0].shadowMap() :
		dirLights_[0].shadowMap();
	pp_.updateInputs(ppInput,
		geomLight_.ldepthTarget().imageView(),
		geomLight_.normalsTarget().imageView(),
		geomLight_.albedoTarget().imageView(),
		ssaoView, ssrView, bloomView, lumView, scatterView, lensView,
		lensDirt, shadowView);

	App::scheduleRerecord();
}

void ViewApp::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	initStaticBuffers(size);
	recreateStaticBufs_ = false;

	// create swapchain framebuffers
	for(auto& buf : bufs) {
		buf.framebuffer = pp_.initFramebuffer(buf.imageView, size);
	}
}

// enable anisotropy if possible
bool ViewApp::features(tkn::Features& enable, const tkn::Features& supported) {
	if(!App::features(enable, supported)) {
		return false;
	}

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

argagg::parser ViewApp::argParser() const {
	// msaa not supported in deferred renderer
	auto parser = App::argParser();
	auto& defs = parser.definitions;
	defs.push_back({
		"model", {"--model"},
		"Path of the gltf model to load (dir must contain scene.gltf)", 1
	});
	defs.push_back({
		"scale", {"--scale"},
		"Apply scale to whole scene", 1
	});
	defs.push_back({
		"maxAniso", {"--maxaniso", "--ani"},
		"Maximum of anisotropy for samplers", 1
	});
	return parser;
}

bool ViewApp::handleArgs(const argagg::parser_results& result, App::Args& bout) {
	if(!App::handleArgs(result, bout)) {
		return false;
	}

	auto& out = static_cast<Args&>(bout);
	if(result.has_option("model")) {
		out.model = result["model"].as<const char*>();
	}
	if(result.has_option("scale")) {
		out.scale = result["scale"].as<float>();
	}
	if(result.has_option("maxAniso")) {
		out.maxAniso = result["maxAniso"].as<float>();
	}

	return true;
}

void ViewApp::record(const RenderBuffer& buf) {
	auto cb = buf.commandBuffer;
	vk::beginCommandBuffer(cb, {});

	timeWidget_.start(cb);

	// render shadow maps
	for(auto& light : pointLights_) {
		if(light.hasShadowMap()) {
			light.render(cb, shadowData_, scene_);
		}
	}
	for(auto& light : dirLights_) {
		light.render(cb, shadowData_, scene_);
	}

	timeWidget_.addTimestamp(cb);

	auto size = swapchainInfo().imageExtent;
	const auto width = size.width;
	const auto height = size.height;
	vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
	vk::cmdSetViewport(cb, 0, 1, vp);
	vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

	RenderData data;
	data.cb = cb;
	data.fb = buf.framebuffer;
	data.size = size;
	frameGraph_.record(data);
	timeWidget_.finish(cb);

	vk::endCommandBuffer(cb);
}

void ViewApp::update(double dt) {
	App::update(dt);
	time_ += dt;

	// movement
	camera_.update(swaDisplay(), dt);

	// TODO: something about potential changes in pass parameters
	auto update = camera_.needsUpdate || updateLights_ || selectionPos_;
	update |= selectionPending_;
	if(update) {
		App::scheduleRedraw();
	}

	// TODO: hack that synchronizes parameters between multiple
	// implementations of same pass...
	geomLight_.aoParams.flags = ao_.params.flags;
	geomLight_.aoParams.factor = ao_.params.factor;
	probe_.geomLight.aoParams.factor = ao_.params.factor;

	// TODO: only here for fps testing, could be removed to not
	// render when not needed
	App::scheduleRedraw();
}

void ViewApp::addPointLight(nytl::Vec3f pos, nytl::Vec3f color, float radius,
		bool shadowMap) {
	auto wb = tkn::WorkBatcher(vkDevice());
	auto& l = pointLights_.emplace_back(wb, shadowData_,
		shadowMap ? vk::ImageView{} : dummyCube_.vkImageView());
	// std::uniform_real_distribution<float> dist(0.0f, 1.0f);
	l.data.position = pos;
	l.data.color = color;
	l.data.radius = radius;
	l.updateDevice();

	tkn::Material lmat;

	// HACK: make sure it doesn't write to depth buffer and isn't
	// rendered into shadow map
	lmat.flags |= tkn::Material::Bit::blend;
	lmat.emissionFac = l.data.color;
	lmat.albedoFac = Vec4f(l.data.color);
	lmat.albedoFac[3] = 1.f;
	l.materialID = scene_.addMaterial(lmat);
	l.instanceID = scene_.addInstance(spherePrimitiveID_,
		tkn::translateMat(pos), l.materialID);

	App::scheduleRerecord();
}

// TODO: command buffer needs to be re-recorded e.g. when lights are added
// TODO: fix environment (uses the main cameras fixed matrix)
//   would probably best to just abolish the ubo in environment
//   and use an externally passed cameraUbo instead, contains
//   fixedMatrix
// TODO: currently y-flipped not sure why
// TODO: some problems with the current probe approach:
//  - the shadow buffers are from the last frame
//  - for cascaded shadow maps (i.e. directional lights), the
//    shadows are messed up outside of the current viewing volume.
//  - due to sharing the shadow buffers with the normal frame rendering
//    this can't happen in parallalel which we require for probes later on
//  - transparent surfaces not ordered correctly on all sides
void ViewApp::screenshot() {
	if(probe_.state == ProbeState::pending) {
		dlg_error("Screenshot already pending");
		return;
	}

	if(probe_.state == ProbeState::invalid) {
		auto& dev = vkDevice();

		// faces, ubo, ds
		// TODO: duplication with cameraUbo_ and cameraDs_
		// TODO(perf): defer initialization for buffers
		auto camUboSize = sizeof(nytl::Mat4f) // proj matrix
			+ sizeof(nytl::Mat4f) // inv proj
			+ sizeof(nytl::Vec3f) // viewPos
			+ 2 * sizeof(float); // near, far plane
		for(auto& face : probe_.faces) {
			face.ubo = {dev.bufferAllocator(), camUboSize,
				vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
			face.ds = {dev.descriptorAllocator(), cameraDsLayout_};

			face.envUbo = {dev.bufferAllocator(), sizeof(nytl::Mat4f),
				vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
			face.envDs = {dev.descriptorAllocator(), cameraDsLayout_};

			vpp::DescriptorSetUpdate dsu(face.ds);
			dsu.uniform(face.ubo);

			vpp::DescriptorSetUpdate edsu(face.envDs);
			edsu.uniform(face.envUbo);
		}

		probe_.retrieve = {dev.bufferAllocator(), probeFaceSize * 6,
			vk::BufferUsageBits::transferDst, dev.hostMemoryTypes(), 8u};

		// passes
		// no cb needed in wb
		auto wb = tkn::WorkBatcher(vkDevice());
		PassCreateInfo passInfo {
			wb,
			depthFormat_, {
				cameraDsLayout_,
				scene_.dsLayout(),
				shadowData_.dsLayout,
			}, {
				linearSampler_,
				nearestSampler_,
			},
			fullVertShader_
		};

		SyncScope empty {};
		SyncScope dstLight;
		dstLight.access = vk::AccessBits::transferRead;
		dstLight.layout = vk::ImageLayout::transferSrcOptimal;
		dstLight.stages = vk::PipelineStageBits::transfer;
		vpp::InitObject initGeomLight(probe_.geomLight, passInfo,
			empty, empty, empty, empty, empty, dstLight, true, true);
		initGeomLight.init();

		// buffers
		GeomLightPass::InitBufferData geomLightBuffers;
		probe_.geomLight.createBuffers(geomLightBuffers, wb, probeSize,
			depthFormat_);
		probe_.geomLight.initBuffers(geomLightBuffers, probeSize,
			env_.irradiance().imageView(),
			env_.envMap().imageView(), env_.convolutionMipmaps(),
			brdfLut_.imageView());

		// cb
		auto& qs = vkDevice().queueSubmitter();
		auto qfam = qs.queue().family();
		probe_.cb = vkDevice().commandAllocator().get(qfam);
		auto cb = probe_.cb.vkHandle();

		vk::beginCommandBuffer(probe_.cb, {});
		vk::Viewport vp{0.f, 0.f,
			(float) probeSize.width, (float) probeSize.height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, probeSize.width, probeSize.height});

		auto lightImg = probe_.geomLight.lightTarget().vkImage();
		for(auto i = 0u; i < 6; ++i) {
			auto& face = probe_.faces[i];

			std::vector<const tkn::PointLight*> pl;
			std::vector<const tkn::DirLight*> dl;
			for(auto& p : pointLights_) pl.push_back(&p);
			for(auto& l : dirLights_) dl.push_back(&l);

			probe_.geomLight.record(cb, probeSize, face.ds, scene_,
				pl, dl, face.envDs, &env_, nullptr);

			vk::BufferImageCopy copy;
			copy.imageSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
			copy.bufferOffset = probe_.retrieve.offset() + probeFaceSize * i;
			copy.imageExtent = {probeSize.width, probeSize.height, 1};
			vk::cmdCopyImageToBuffer(cb, lightImg,
				vk::ImageLayout::transferSrcOptimal, probe_.retrieve.buffer(),
				{{copy}});

			vk::ImageMemoryBarrier barrier;
			barrier.image = lightImg;
			barrier.srcAccessMask = vk::AccessBits::transferRead;
			barrier.oldLayout = vk::ImageLayout::transferSrcOptimal;
			// TODO: probably cleared next, right? what access/stage is that,
			// probably transfer or something?
			barrier.dstAccessMask = vk::AccessBits::memoryWrite;
			barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
			barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
			vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
				vk::PipelineStageBits::allCommands, {}, {}, {}, {{barrier}});
		}

		vk::endCommandBuffer(probe_.cb);
	}

	// update ubo for camera
	for(auto i = 0u; i < 6u; ++i) {
		auto map = probe_.faces[i].ubo.memoryMap();
		auto span = map.span();

		auto mat = tkn::cubeProjectionVP(camera_.position(), i);
		auto inv = nytl::Mat4f(nytl::inverse(mat));
		tkn::write(span, mat);
		tkn::write(span, inv);
		tkn::write(span, camera_.position());
		tkn::write(span, 0.01f); // near
		tkn::write(span, 30.f); // far
		map.flush();

		// fixed matrix, position irrelevant
		auto envMap = probe_.faces[i].envUbo.memoryMap();
		auto envSpan = envMap.span();
		tkn::write(envSpan, tkn::cubeProjectionVP({}, i));
		envMap.flush();
	}

	probe_.state = ProbeState::pending;
}

bool ViewApp::key(const swa_key_event& ev) {
	static std::default_random_engine rnd(std::time(nullptr));
	if(App::key(ev)) {
		return true;
	}

	if(!ev.pressed) {
		return false;
	}

	auto numModes = 14u;
	switch(ev.keycode) {
		case swa_key_m: // move light here
			if(!dirLights_.empty()) {
				dirLights_[0].data.dir = -nytl::normalized(camera_.position());
			} else {
				pointLights_[0].data.position = camera_.position();
			}
			updateLights_ = true;
			return true;
		case swa_key_p:
			if(!dirLights_.empty()) {
				dirLights_[0].data.flags ^= tkn::lightFlagPcf;
			} else {
				pointLights_[0].data.flags ^= tkn::lightFlagPcf;
			}
			updateLights_ = true;
			return true;
		case swa_key_v:
			camera_.perspectiveFov(*camera_.perspectiveFov() * 0.98);
			camera_.needsUpdate = true;
			break;
		case swa_key_b:
			camera_.perspectiveFov(*camera_.perspectiveFov() / 0.98);
			camera_.needsUpdate = true;
			break;
		case swa_key_n:
			debugMode_ = (debugMode_ + 1) % numModes;
			recreatePasses_ = true; // recreate, rerecord
			return true;
		case swa_key_l:
			debugMode_ = (debugMode_ + numModes - 1) % numModes;
			recreatePasses_ = true; // recreate, rerecord
			return true;
		case swa_key_equals:
			desiredLuminance_ *= 1.1f;
			return true;
		case swa_key_minus:
			desiredLuminance_ /= 1.1f;
			return true;
		case swa_key_t: { // screenshot
			screenshot();
			return true;
		} case swa_key_comma: {
			std::uniform_real_distribution<float> dist(0.0f, 1.0f);
			addPointLight(camera_.position(), {dist(rnd), dist(rnd), dist(rnd)}, 0.5, false);
			break;
		 } case swa_key_z: {
			std::uniform_real_distribution<float> dist(0.0f, 1.0f);
			addPointLight(camera_.position(), {dist(rnd), dist(rnd), dist(rnd)}, 5.0, true);
			break;
		} case swa_key_f: {
			hideTimeWidget_ ^= true;
			timeWidget_.hide(hideTimeWidget_);
			break;
		} default:
			break;
	}

	auto uk = static_cast<unsigned>(ev.keycode);
	auto k1 = static_cast<unsigned>(swa_key_k1);
	auto k0 = static_cast<unsigned>(swa_key_k0);
	if(uk >= k1 && uk < k0) {
		auto diff = (1 + uk - k1) % numModes;
		debugMode_ = diff;
		recreatePasses_ = true; // recreate, rerecord
	} else if(ev.keycode == swa_key_k0) {
		debugMode_ = 0;
		recreatePasses_ = true; // recreate, rerecord
	}

	return false;
}

void ViewApp::mouseMove(const swa_mouse_move_event& ev) {
	App::mouseMove(ev);
	camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
}

bool ViewApp::mouseButton(const swa_mouse_button_event& ev) {
	if(App::mouseButton(ev)) {
		return true;
	}

	if(ev.pressed && ev.button == swa_mouse_button_right) {
		auto ipos = nytl::Vec2i {ev.x, ev.y};
		auto& ie = swapchainInfo().imageExtent;
		if(ipos.x >= 0 && ipos.y >= 0) {
			auto pos = nytl::Vec2ui(ipos);
			if(pos.x <= ie.width && pos.y < ie.height) {
				selectionPos_ = pos;
			}
		}
	}

	return false;
}

void ViewApp::updateDevice() {
	if(recreatePasses_) {
		recreatePasses_ = false;
		auto wb = tkn::WorkBatcher(vkDevice());
		auto& qs = vkDevice().queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = vkDevice().commandAllocator().get(qfam);
		wb.cb = cb;

		vk::beginCommandBuffer(cb, {});
		initPasses(wb);

		// initPasses reloads all pipelines, shouldn't happen in
		// normal rendering. So waiting here shouldn't be problem
		vk::endCommandBuffer(cb);
		auto id = qs.add(cb);
		qs.wait(id);

		recreateStaticBufs_ = true; // recreate buffers and stuff
	}

	auto near = -camera_.near();
	auto far = -camera_.far();

	if(recreateStaticBufs_ && renderer().swapchain()) {
		auto e = swapchainInfo().imageExtent;
		initStaticBuffers(e);
		recreateStaticBufs_ = false;
	}

	geomLight_.updateDevice();
	if(renderPasses_ & passBloom) {
		bloom_.updateDevice();
	}
	if(renderPasses_ & passSSR) {
		ssr_.updateDevice();
	}
	if(renderPasses_ & passAO) {
		ao_.updateDevice();
	}
	if(renderPasses_ & passCombine) {
		combine_.updateDevice();
	}
	if(renderPasses_ & passScattering) {
		scatter_.updateDevice();
	}
	if(renderPasses_ & passHighlight) {
		highlight_.updateDevice();
	}
	if(renderPasses_ & passLens) {
		lens_.updateDevice();
	}
	pp_.updateDevice();

	// TODO: needed from update stage. Probably better to do all calculations
	// just there though probably
	float dt = 1 / 60.f;

	// update dof focus
	if(pp_.params.flags & pp_.flagDOF) {
		auto map = renderStage_.memoryMap();
		map.invalidate();
		auto span = map.span();

		auto focus = float(tkn::read<f16>(span));
		gui_.focusDepth->label(std::to_string(focus));
		pp_.params.depthFocus = nytl::mix(pp_.params.depthFocus, focus, dt * 30);
		gui_.dofDepth->label(std::to_string(pp_.params.depthFocus));
	} else {
		gui_.dofDepth->label("-");
	}

	// exposure
	if(renderPasses_ & passLuminance) {
		auto lum = luminance_.updateDevice();
		gui_.luminance->label(std::to_string(lum));

		// NOTE: really naive approach atm
		lum *= pp_.params.exposure;
		float diff = desiredLuminance_ - lum;
		pp_.params.exposure += 10 * dt * diff;
		gui_.exposure->label(std::to_string(pp_.params.exposure));
	} else {
		pp_.params.exposure = desiredLuminance_ / 0.2; // rather random
		gui_.luminance->label("-");
		gui_.exposure->label(std::to_string(pp_.params.exposure));
	}

	// update scene ubo
	if(camera_.needsUpdate) {
		camera_.needsUpdate = false;
		auto map = cameraUbo_.memoryMap();
		auto span = map.span();
		auto mat = camera_.viewProjectionMatrix();
		tkn::write(span, mat);
		tkn::write(span, nytl::Mat4f(nytl::inverse(mat)));
		tkn::write(span, camera_.position());
		tkn::write(span, near);
		tkn::write(span, far);
		if(!map.coherent()) {
			map.flush();
		}

		auto envMap = envCameraUbo_.memoryMap();
		auto envSpan = envMap.span();
		tkn::write(envSpan, camera_.fixedViewProjectionMatrix());
		if(!envMap.coherent()) {
			envMap.flush();
		}

		if(updateScene_) {
			auto semaphore = scene_.updateDevice(mat);
			if(semaphore) {
				addSemaphore(semaphore, vk::PipelineStageBits::allGraphics);
				App::scheduleRerecord();
			}
		}

		// depend on camera matrix
		updateLights_ = true;
	}

	if(updateLights_) {
		for(auto& l : pointLights_) {
			l.updateDevice();
		}
		for(auto& l : dirLights_) {
			l.updateDevice(camera_.viewProjectionMatrix(), near, far);
		}
		updateLights_ = false;
	}

	if(selectionPending_) {
		auto bytes = vpp::retrieveMap(selectionStage_,
			vk::Format::r32g32b32a32Sfloat,
			{1u, 1u, 1u}, {vk::ImageAspectBits::color});
		dlg_assert(bytes.size() == sizeof(float) * 4);
		auto fp = reinterpret_cast<const float*>(bytes.data());
		int selected = 65536 * (0.5f + 0.5f * fp[2]);
		selectionPending_ = {};

		auto& primitives = scene_.primitives();
		bool found = false;
		if(selected <= 0) {
			dlg_warn("Non-positive selected: {}", selected);
		} else if(unsigned(selected) < primitives.size()) {
			dlg_info("Selected primitive: {}", selected);
		} else {
			selected -= primitives.size();
			if(unsigned(selected) < dirLights_.size()) {
				dlg_info("Selected dir light: {}", selected);
			} else {
				selected -= dirLights_.size();
				if(unsigned(selected) < pointLights_.size()) {
					found = true;
					dlg_info("Selected point light: {}", selected);
					if(selected_.id == 0xFFFFFFFFu) {
						vuiPanel_->add(std::move(selected_.removedFolder));
					}
					selected_.id = selected;
					auto& l = pointLights_[selected];
					selected_.radiusField->utf8(std::to_string(l.data.radius));
					selected_.a1Field->utf8(std::to_string(l.data.attenuation.y));
					selected_.a2Field->utf8(std::to_string(l.data.attenuation.z));
				} else {
					dlg_warn("Invalid selection value: {}", selected);
				}
			}
		}

		if(!found && selected_.id != 0xFFFFFFFFu) {
			selected_.id = 0xFFFFFFFFu;
			selected_.removedFolder = vuiPanel_->remove(*selected_.folder);
		}
	}

	// recevie selection id
	// read the one pixel from the normals buffer (containing the primitive id)
	// where the mouse was pressed
	if(selectionPos_) {
		// TODO: readd this
		/*
		auto pos = *selectionPos_;
		selectionPos_ = {};

		vk::beginCommandBuffer(selectionCb_, {});
		vpp::changeLayout(selectionCb_, gpass_.normal.image(),
			vk::ImageLayout::shaderReadOnlyOptimal, // TODO: wouldn't work in first frame...
			vk::PipelineStageBits::topOfPipe, {},
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});
		vk::ImageBlit blit;
		blit.srcOffsets[0] = {int(pos.x), int(pos.y), 0u};
		blit.srcOffsets[1] = {int(pos.x + 1), int(pos.y + 1), 1u};
		blit.srcSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
		blit.dstOffsets[1] = {1u, 1u, 1u};
		blit.dstSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
		vk::cmdBlitImage(selectionCb_,
			gpass_.normal.image(),
			vk::ImageLayout::transferSrcOptimal,
			selectionStage_,
			vk::ImageLayout::general,
			{{blit}}, vk::Filter::nearest);
		vpp::changeLayout(selectionCb_, gpass_.normal.image(),
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			vk::ImageLayout::colorAttachmentOptimal,
			vk::PipelineStageBits::colorAttachmentOutput,
			vk::AccessBits::colorAttachmentWrite,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});
		// TODO: we probably also need a barrier for selectionStage_,
		// right? then also change it to transferDstOptimal for blit...
		vk::endCommandBuffer(selectionCb_);

		vk::SubmitInfo si;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &selectionCb_.vkHandle();
		si.pSignalSemaphores = &selectionSemaphore_.vkHandle();
		si.signalSemaphoreCount = 1u;
		vkDevice().queueSubmitter().add(si);

		// wait for blit to finish before continuing with rendering
		App::addSemaphore(selectionSemaphore_,
			vk::PipelineStageBits::colorAttachmentOutput);
		selectionPending_ = true;
		*/
	}

	timeWidget_.updateDevice();

	// screenshot
	// TODO: inefficient, don't stall!
	if(probe_.state == ProbeState::pending) {
		probe_.geomLight.updateDevice();
		auto& qs = vkDevice().queueSubmitter();
		qs.wait(qs.add(probe_.cb));

		auto map = probe_.retrieve.memoryMap();
		if(!map.coherent()) {
			map.invalidate();
		}

		dlg_assert(map.size() == 6 * probeFaceSize);

		const auto* const ptr = map.ptr();
		auto ptrs = {
			ptr + 0 * probeFaceSize,
			ptr + 1 * probeFaceSize,
			ptr + 2 * probeFaceSize,
			ptr + 3 * probeFaceSize,
			ptr + 4 * probeFaceSize,
			ptr + 5 * probeFaceSize,
		};

		auto format = GeomLightPass::lightFormat;
		auto provider = tkn::wrapImage({probeSize.width, probeSize.height, 1u},
			format, 1, 1, {ptrs}, true);
		auto res = tkn::writeKtx("probe.ktx", *provider);
		dlg_assertm(res == tkn::WriteError::none, (int) res);
		dlg_info("written");

		probe_.state = ProbeState::initialized;
	}

	// NOTE: not sure where to put that. after rvg label changes here
	// because it updates rvg
	App::updateDevice();
}

void ViewApp::resize(unsigned width, unsigned height) {
	App::resize(width, height);

	selectionPos_ = {};
	camera_.aspect({width, height});

	// update time widget position
	auto pos = nytl::Vec2ui{width, height};
	pos.x -= 240;
	pos.y = 10;
	timeWidget_.move(nytl::Vec2f(pos));
}

int main(int argc, const char** argv) {
	return tkn::appMain<ViewApp>(argc, argv);
}
