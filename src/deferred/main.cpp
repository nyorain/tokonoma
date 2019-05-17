#include <stage/app.hpp>
#include <stage/camera.hpp>
#include <stage/app.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>
#include <stage/defer.hpp>
#include <stage/scene/shape.hpp>
#include <stage/scene/light.hpp>
#include <stage/scene/scene.hpp>
#include <stage/scene/skybox.hpp>
#include <argagg.hpp>

#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <vui/textfield.hpp>
#include <vui/colorPicker.hpp>

#include <ny/appContext.hpp>

#include <ny/key.hpp>
#include <ny/mouseButton.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/debug.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/formats.hpp>
#include <vpp/init.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>
#include <vpp/handles.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/imageOps.hpp>

#include <shaders/stage.fullscreen.vert.h>
#include <shaders/deferred.gbuf.vert.h>
#include <shaders/deferred.gbuf.frag.h>
#include <shaders/deferred.pointLight.frag.h>
#include <shaders/deferred.pointLight.vert.h>
#include <shaders/deferred.dirLight.frag.h>
#include <shaders/deferred.ssao.frag.h>
#include <shaders/deferred.ssaoBlur.frag.h>
#include <shaders/deferred.pointScatter.frag.h>
#include <shaders/deferred.dirScatter.frag.h>
#include <shaders/deferred.gblur.comp.h>
#include <shaders/deferred.ssr.comp.h>
#include <shaders/deferred.combine.comp.h>
#include <shaders/deferred.pp.frag.h>
#include <shaders/deferred.debug.frag.h>

#include <cstdlib>
#include <random>

// TODO: maybe do high pass on light output *after* blending.
//   currently, when there are many light shining on a surface
//   the surface gets bright (obviously) but is not added to
//   bloom buffer since no *single* light passes the threshold.
//   requires additional pass though
// TODO: re-add light scattering (per light?)
//   even if per-light we can still apply if after lightning pass
//   probably best to just use one buffer for all lights though, right?
//   probably not a bad idea to use that buffer with additional blending
//   as additional attachment in the light pass
// TODO: we should be able to always use the same attenuation (or at least
//   make it independent from radius) by normalizing distance from light
//   (dividing by radius) before caluclating attunation
// TODO: re-add skybox and transparent objects in forward pass.
//   the objects from that pass will have no effect on ssao or ssr,
//   no other way to do it. That means that you will never see a transparent
//   object in a reflection... (todo: you should see the skybox though!
//   how to handle that?)
//   An alternative idea is to use 2 forward passes: one for almost opaque
//   objects that can be aproximated as opaque for ssr/ssao sakes and then
//   (after reading the buffers for ss algorithms) another forward pass
//   for almost-transparent objects. Probably not worht it though
// TODO: parameterize all (currently somewhat random) values and factors
//   (especially in artefact-prone screen space shaders)
// TODO: re-add (fix for point light) the shadowmap-based light
//   scattering approach
// TODO: rgba16snorm (normalsFormat) isn't guaranteed to be supported
//   as color attachment... i guess we could fall back to rgba16sint
//   which is guaranteed to be supported? and then encode it
// TODO: point light scattering currently doesn't support the ldv value,
//   i.e. view-dir dependent scattering only based on attenuation
// TODO: support light scattering for every light (add a light flag for it).
//   For point lights, we only have to scatter in the light volume.
//   Probably can't combine it with the volume rasterization step in the
//   light pass though, so have to render the box twice.
// TODO: cascaded shadow maps for directional lights
//   Allow to set the shadow map size for all lights. Or automatically
//   dynamically allocate shadow maps per frame (but also see the
//   only one shadow map at all thing).
// TODO: fix theoretical synchronization issues
// TODO: do we really need depth mip levels anywhere? test if it really
//   brings ssao performance improvement. Otherwise we are wasting this
//   whole "depth mip map levels" thing
// TODO: blur ssr in extra pass (with compute shader shared memory,
//   see ssrBlur.comp wip). Should hopefully improve performance
//   and allow for greater blur. Also distribute factors (compute guass +
//   depth difference) via shared variables
// TODO: how to handle light attenuation for point lights?
//   see scene.glsl:attenuation, does it make sense to give each light
//   its own parameters? normalize distance by radius before passing
//   it to attenuation? parameters that work fine for a normalized
//   distance are (1, 8, 128), but then lights are rather weak

// low prio:
// TODO: look at adding smaa. That needs multiple post processing
//   passes though... Add it as new class
// TODO: more an idea:
//   shadow cube debug mode (where cubemap is rendered, one can
//   look around). Maybe use moving the camera then for moving the light.
//   can be activated via vui when light is selcted.
//   something comparable for dir lights? where direction can be set by
//   moving camera around?

// lower prio optimizations:
// TODO(optimization): look into textureOffset/textureLodOffset functions for
//   shaders. We use this functionality really often, might get
//   something for free there
// TODO(optimization): when shaderStorageImageExtendedFormats is supported,
//   we can perform pretty much all fullscreen passes (ssao, ssr, combining,
//   ssao blurring) as compute shaders. For doing it with post processing/
//   tonemapping we'd also need to create the swapchain with storage image
//   caps *and* know that it uses a format that is supported... so probably
//   stick with fullscreen render pass there.
//   Some of the above mentioned don't need extended formats, those should
//   definietly be moved to compute passes.
//   NOTE: for some passes (and their inputs) that means giving up
//   input attachments though. Is that a performance loss anywhere?
//   input attachments are probably only relevent when using multiple
//   subpasses, right?
// TODO(optimization): try out other mechanisms for better ssao cache coherency
//   we should somehow use a higher mipmap level i guess
// TODO(optimization, cleanup): we only need emission as input to lightning pass since its
//   w component has a pbr property. We could instead move that to
//   normals and the material id of normals somewhere else, e.g.
//   the emission buffer. But that is sfloat instead of snorm,
//   not sure how to encode in that case.
// TODO(optimization?): we could theoretically just use one shadow map at all.
//   requires splitting the light passes though (rendering with light pipe)
//   so might have disadvantage. so not high prio
// TODO(optimization?) we could reuse float gbuffers for later hdr rendering
//   not that easy though since normal buffer has snorm format...
//   attachments can be used as color *and* input attachments in a
//   subpass. And reusing the normals buffer as light output buffer
//   is pretty perfect, no pass depends on both as input (edit: ssr
//   does now. remove that if it's not needed or do ssr
//   before the light pass then).
//   should be possible with vulkan mutable format images
// TODO(optimization): more efficient point light shadow cube map
//   rendering: only render those sides that we can actually see...
//   -> nothing culling-related implement at all at the moment
// TODO(optimization): we currently don't use the a component of the
//   bloom targets. We could basically get a free (strong) blur there...
//   maybe good for ssao or something?
// TODO: look into vk_khr_multiview (promoted to vulkan 1.1?) for
//   cubemap rendering (point lights, but also skybox transformation)
// idea: just treat light scattering as emission? get the strong blur for free.
//   probably way too strong though...

// NOTE: investigate/compare deferred lightning elements?
//   http://gameangst.com/?p=141
// NOTE: we always use ssao, even when object/material has ao texture.
//   In that case both are multiplied. That's how its usually done, see
//   https://docs.unrealengine.com/en-us/Engine/Rendering/LightingAndShadows/AmbientOcclusion
// NOTE: ssao currently independent from *any* lights at all
// NOTE: we currently completely ignore tangents and compute that
//   in the shader via derivates
// NOTE: could investigate reversed z buffer
//   http://dev.theomader.com/depth-precision/
//   https://nlguillemot.wordpress.com/2016/12/07/reversed-z-in-opengl/
// NOTE: ssr needs hdr format since otherwise we get artefacts for storing
//   the uv value when the window has more than 255 pixels.
// NOTE: we currently apply the base lod (mipmap level 0) of the emission
//   texture in the combine pass (before ssr) but only apply the other
//   emission lods (blooms, mipmap levels >0) after/together with ssr
//   in the pp pass. Bloom and ssr don't work well together but we still
//   want emissive objects to be reflected. They just won't have bloom
//   in the reflection. Could be fixed by reading the emission buffer
//   during ssr but that's probably really not worth it!

class ViewApp : public doi::App {
public:
	using Vertex = doi::Primitive::Vertex;

	static constexpr auto lightFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto normalsFormat = vk::Format::r16g16b16a16Snorm;
	static constexpr auto albedoFormat = vk::Format::r8g8b8a8Srgb;
	static constexpr auto emissionFormat = vk::Format::r16g16b16a16Sfloat;
	// static constexpr auto emissionFormat = vk::Format::r8Unorm;
	static constexpr auto ssaoFormat = vk::Format::r8Unorm;
	static constexpr auto scatterFormat = vk::Format::r8Unorm;
	static constexpr auto ldepthFormat = vk::Format::r16Sfloat;
	// static constexpr auto ldepthFormat = vk::Format::r8Unorm;
	// static constexpr auto ssrFormat = vk::Format::r32g32b32a32Sfloat;
	static constexpr auto ssrFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto ssaoSampleCount = 16u;
	static constexpr auto pointLight = true;
	static constexpr auto maxBloomLevels = 4u;

	// pp.frag
	static constexpr unsigned flagSSAO = (1u << 0u);
	static constexpr unsigned flagScattering = (1u << 1u);
	static constexpr unsigned flagSSR = (1u << 2u);
	static constexpr unsigned flagBloom = (1u << 3u);
	static constexpr unsigned flagFXAA = (1u << 4u);
	static constexpr unsigned flagDiffuseIBL = (1u << 5u);

	static constexpr unsigned modeNormal = 0u;
	static constexpr unsigned modeAlbedo = 1u;
	static constexpr unsigned modeNormals = 2u;
	static constexpr unsigned modeRoughness = 3u;
	static constexpr unsigned modeMetalness = 4u;
	static constexpr unsigned modeAO = 5u;
	static constexpr unsigned modeAlbedoAO = 6u;
	static constexpr unsigned modeSSR = 7u;
	static constexpr unsigned modeDepth = 8u;
	static constexpr unsigned modeEmission = 9u;
	static constexpr unsigned modeBloom = 10u;

	static constexpr unsigned timestampCount = 11u;

	// see various post processing or screen space shaders
	struct RenderParams {
		std::uint32_t mode = 0u;
		std::uint32_t flags {flagFXAA};
		float aoFactor {0.05f};
		float ssaoPow {3.f};
		std::uint32_t tonemap {2};
		float exposure {1.f};
	};

	static const vk::PipelineColorBlendAttachmentState& noBlendAttachment();

public:
	struct SSAOInitData {
		vpp::SubBuffer samplesStage;
		vpp::SubBuffer noiseStage;
	};

	bool init(const nytl::Span<const char*> args) override;
	void initRenderData() override;

	void initGPass();
	void initLPass();
	void initPPass();
	SSAOInitData initSSAO(const doi::WorkBatcher& batcher);
	void initSSAOBlur();
	void initScattering();
	void initBloom();
	void initSSR();
	void initCombine();
	void initDebug();
	void initFwd();
	void updateDescriptors();

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
	const char* name() const override { return "Deferred Renderer"; }
	std::pair<vk::RenderPass, unsigned> rvgPass() const override {
		return {pp_.pass, 0u};
	}

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

	bool updateRenderParams_ {true};
	RenderParams params_;
	vpp::SubBuffer paramsUbo_;
	std::uint32_t bloomLevels_ {};

	vpp::Sampler linearSampler_;
	vpp::Sampler nearestSampler_;

	doi::Texture dummyTex_;
	doi::Texture dummyCube_;
	doi::Scene scene_;
	std::optional<doi::Material> lightMaterial_;

	std::string modelname_ {};
	float sceneScale_ {1.f};
	float maxAnisotropy_ {16.f};

	bool anisotropy_ {}; // whether device supports anisotropy
	float time_ {};
	bool rotateView_ {}; // mouseLeft down
	doi::Camera camera_ {};
	bool updateLights_ {true};
	bool updateDescriptors_ {false};

	doi::Skybox skybox_;

	// needed for point light rendering.
	// TODO: also needed for skybox
	vpp::SubBuffer boxIndices_;

	// image view into the depth buffer that accesses all depth levels
	vpp::ImageView depthMipView_;
	unsigned depthMipLevels_ {};

	// 1x1px host visible image used to copy the selected model id into
	vpp::Image selectionStage_;
	std::optional<nytl::Vec2ui> selectionPos_;
	vpp::CommandBuffer selectionCb_;
	vpp::Semaphore selectionSemaphore_;
	bool selectionPending_ {};

	vpp::QueryPool queryPool_ {};
	std::uint32_t timestampBits_ {};

	// TODO: add vpp methods to merge them to one (or into the default
	// one) after initialization
	struct Alloc {
		vpp::DeviceMemoryAllocator memHost;
		vpp::DeviceMemoryAllocator memDevice;

		vpp::BufferAllocator bufHost;
		vpp::BufferAllocator bufDevice;

		Alloc(const vpp::Device& dev) :
			memHost(dev), memDevice(dev),
			bufHost(memHost), bufDevice(memDevice) {}
	};

	std::optional<Alloc> alloc;

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

	// gbuffer and lightning passes
	struct {
		vpp::RenderPass pass;
		vpp::Framebuffer fb;
		vpp::ViewableImage normal;
		vpp::ViewableImage albedo;
		vpp::ViewableImage emission;
		vpp::ViewableImage depth; // color, for linear sampling/mipmaps
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} gpass_;

	// lightning pass
	struct {
		vpp::RenderPass pass;
		vpp::Framebuffer fb;
		vpp::TrDsLayout dsLayout; // input
		vpp::TrDs ds;
		vpp::ViewableImage light;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pointPipe;
		vpp::Pipeline dirPipe;
	} lpass_;

	// post processing
	struct {
		vpp::RenderPass pass;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::Pipeline pipe;
	} pp_;

	// ssao
	struct {
		vpp::RenderPass pass;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::ViewableImage target;
		doi::Texture noise;
		vpp::Framebuffer fb;
		vpp::SubBuffer samples;

		struct {
			vpp::Framebuffer fb;
			vpp::ViewableImage target;
			vpp::TrDsLayout dsLayout;
			vpp::TrDs dsHorz;
			vpp::TrDs dsVert;
			vpp::PipelineLayout pipeLayout;
			vpp::Pipeline pipe;
		} blur;
	} ssao_;

	// light scattering
	struct {
		vpp::RenderPass pass;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pointPipe;
		vpp::Pipeline dirPipe;

		// TODO: we might want to allow per-light scattering
		vpp::ViewableImage target;
		vpp::Framebuffer fb;
	} scatter_;

	// bloom
	struct BloomLevel {
		vpp::ImageView view;
		vpp::TrDs ds;
	};

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::Image tmpTarget;
		std::vector<BloomLevel> tmpLevels;
		std::vector<BloomLevel> emissionLevels;
		vpp::ImageView fullBloom;
	} bloom_;

	// ssr
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::ViewableImage target;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} ssr_;

	// combining compute pass
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} combine_;

	// debug render pass, uses pp_.pass
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} debug_;

	// forward pass
	struct {
		vpp::RenderPass pass;
		vpp::Framebuffer fb;
		// vpp::TrDsLayout dsLayout;
		// vpp::TrDs ds;
		// vpp::PipelineLayout pipeLayout;
		// vpp::Pipeline pipe;
	} fwd_;

	// times
	struct TimePass {
		rvg::Text passName;
		rvg::Text time;
	};

	struct {
		std::array<TimePass, timestampCount> texts;
		rvg::RectShape bg;
		rvg::Paint bgPaint;
		rvg::Paint fgPaint;
		unsigned frameCount {0};
		bool hidden {};
	} timings_;
};

bool ViewApp::init(const nytl::Span<const char*> args) {
	if(!doi::App::init(args)) {
		return false;
	}

	camera_.perspective.near = 0.01f;
	camera_.perspective.far = 20.f;

	// gui
	auto& gui = this->gui();

	using namespace vui::dat;
	auto pos = nytl::Vec2f {100.f, 0};
	auto& panel = gui.create<vui::dat::Panel>(pos, 300.f);

	auto& pp = panel.create<vui::dat::Folder>("Post Processing");
	auto createNumTextfield = [](auto& at, auto name, auto initial, auto func) {
		auto start = std::to_string(initial);
		if(start.size() > 4) {
			start.resize(4, '\0');
		}
		auto& t = at.template create<Textfield>(name, start).textfield();
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

	auto flag = &updateRenderParams_;
	createValueTextfield(pp, "exposure", params_.exposure, flag);
	createValueTextfield(pp, "aoFactor", params_.aoFactor, flag);
	createValueTextfield(pp, "ssaoPow", params_.ssaoPow, flag);
	createValueTextfield(pp, "tonemap", params_.tonemap, flag);

	auto& cb1 = pp.create<Checkbox>("ssao").checkbox();
	cb1.set(params_.flags & flagSSAO);
	cb1.onToggle = [&](auto&) {
		params_.flags ^= flagSSAO;
		updateRenderParams_ = true;
		App::resize_ = true; // recreate
		App::scheduleRerecord();
	};

	// auto& cb2 = pp.create<Checkbox>("scattering").checkbox();
	// cb2.set(pp_.params.flags & flagScattering);
	// cb2.onToggle = [&](auto&) {
	// 	pp_.params.flags ^= flagScattering;
	// 	pp_.updateParams = true;
	// 	updateDescriptors_ = true;
	// 	App::scheduleRerecord();
	// };

	auto& cb3 = pp.create<Checkbox>("SSR").checkbox();
	cb3.set(params_.flags & flagSSR);
	cb3.onToggle = [&](auto&) {
		params_.flags ^= flagSSR;
		updateRenderParams_ = true;
		App::resize_ = true; // recreate
		App::scheduleRerecord();
	};

	auto& cb4 = pp.create<Checkbox>("Bloom").checkbox();
	cb4.set(params_.flags & flagBloom);
	cb4.onToggle = [&](auto&) {
		params_.flags ^= flagBloom;
		updateRenderParams_ = true;
		App::resize_ = true; // recreate
		App::scheduleRerecord();
	};

	auto& cb5 = pp.create<Checkbox>("FXAA").checkbox();
	cb5.set(params_.flags & flagFXAA);
	cb5.onToggle = [&](auto&) {
		params_.flags ^= flagFXAA;
		updateRenderParams_ = true;
	};

	auto& cb6 = pp.create<Checkbox>("Diffuse IBL").checkbox();
	cb6.set(params_.flags & flagDiffuseIBL);
	cb6.onToggle = [&](auto&) {
		params_.flags ^= flagDiffuseIBL;
		updateRenderParams_ = true;
	};

	// light selection
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

	// times widget
	{
		auto names = {
			"shadows:",
			"gpass:",
			"depth mips:",
			"ssao:",
			"scatter:",
			"lpass:",
			"fwd:",
			"bloom:",
			"ssr:",
			"combine:",
			"post process:"
		};

		timings_.bgPaint = {rvgContext(), rvg::colorPaint({20, 20, 20, 200})};
		timings_.fgPaint = {rvgContext(), rvg::colorPaint({230, 230, 230, 255})};
		auto dm = rvg::DrawMode {};
		dm.deviceLocal = true;
		dm.fill = true;
		dm.stroke = 0.f;
		dm.aaFill = false;
		timings_.bg = {rvgContext(), {}, {}, dm};

		// positions set by resize
		for(auto i = 0u; i < timings_.texts.size(); ++i) {
			auto name = *(names.begin() + i);
			timings_.texts[i].passName = {rvgContext(), {}, name, defaultFont(), 14.f};
			timings_.texts[i].time = {rvgContext(), {}, "", defaultFont(), 14.f};
		}
	}

	return true;
}

void ViewApp::initRenderData() {
	auto& dev = vulkanDevice();

	// initialization setup
	// allocator
	alloc.emplace(dev);
	auto& alloc = *this->alloc;

	// stage allocators only valid during this function
	vpp::DeviceMemoryAllocator memStage(dev);
	vpp::BufferAllocator bufStage(memStage);

	// command buffer
	auto& qs = dev.queueSubmitter();
	auto qfam = qs.queue().family();
	auto cb = dev.commandAllocator().get(qfam);
	vk::beginCommandBuffer(cb, {});

	// create batch
	doi::WorkBatcher batch{dev, cb, {
			alloc.memDevice, alloc.memHost, memStage,
			alloc.bufDevice, alloc.bufHost, bufStage,
			dev.descriptorAllocator(),
		}
	};

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
	// sci.anisotropyEnable = anisotropy_;
	// sci.maxAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
	linearSampler_ = {dev, sci};
	vpp::nameHandle(linearSampler_, "linearSampler");

	sci.magFilter = vk::Filter::nearest;
	sci.minFilter = vk::Filter::nearest;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	sci.anisotropyEnable = false;
	nearestSampler_ = {dev, sci};
	vpp::nameHandle(linearSampler_, "nearestSampler");

	initGPass(); // geometry to g buffers
	initLPass(); // per light: using g buffers for shading
	initPPass(); // post processing, combining
	auto ssaoData = initSSAO(batch); // ssao
	initSSAOBlur(); // gaussian blur pass over ssao
	initScattering(); // light scattering/volumentric light shafts/god rays
	initBloom(); // blur light regions
	initSSR(); // screen space relfections
	initCombine(); // combining pass
	initDebug(); // init debug pipeline/pass
	initFwd(); // init forward pass for skybox and transparent objects

	// dummy texture for materials that don't have a texture
	// TODO: we could just create the dummy cube and make the dummy
	// texture just a view into one of the dummy cube faces...
	// TODO: those are required below (mainly by lights and by
	//   materials, both should be fixable).
	auto data = std::make_unique<std::byte[]>(4);
	auto idata = std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u};
	std::memcpy(data.get(), idata.data(), 4u);

	std::vector<std::unique_ptr<std::byte[]>> faces;
	faces.emplace_back(std::move(data));
	// auto initDummyTex = doi::Initializer<doi::Texture>(batch,
	// 	std::move(faces), vk::Format::r8g8b8a8Unorm,
	// 	vk::Extent2D{1u, 1u});
	dummyTex_ = {batch, std::move(faces), vk::Format::r8g8b8a8Unorm,
		vk::Extent2D{1u, 1u}};
	vpp::nameHandle(dummyTex_.image(), "dummyTex.image");
	vpp::nameHandle(dummyTex_.imageView(), "dummyTex.view");

	faces.resize(6);
	for(auto& f : faces) {
		f = std::make_unique<std::byte[]>(4);
		std::memcpy(f.get(), idata.data(), 4u);
	}

	auto params = doi::TextureCreateParams {};
	params.cubemap = true;
	dummyCube_ = {batch, std::move(faces), vk::Format::r8g8b8a8Unorm,
		vk::Extent2D{1u, 1u}, params};
	vpp::nameHandle(dummyCube_.image(), "dummyCube.image");
	vpp::nameHandle(dummyCube_.imageView(), "dummyCube.view");
	// auto initDummyCube = doi::Initializer<doi::Texture>(batch,
	// 	std::move(faces), vk::Format::r8g8b8a8Unorm, vk::Extent2D{1u, 1u});

	// initDummyTex.alloc(batch);
	// initDummyCube.alloc(batch);
	// dummyTex_ = initDummyTex.finish(batch);
	// dummyCube_ = initDummyCube.finish(batch);

	// ubo and stuff
	auto sceneUboSize = sizeof(nytl::Mat4f) // proj matrix
		+ sizeof(nytl::Mat4f) // inv proj
		+ sizeof(nytl::Vec3f) // viewPos
		+ 2 * sizeof(float); // near, far plane
	vpp::Init<vpp::TrDs> initSceneDs(batch.alloc.ds, sceneDsLayout_);
	vpp::Init<vpp::SubBuffer> initSceneUbo(batch.alloc.bufHost, sceneUboSize,
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes());

	shadowData_ = doi::initShadowData(dev, depthFormat(),
		lightDsLayout_, materialDsLayout_, primitiveDsLayout_,
		doi::Material::pcr());

	// params ubo
	paramsUbo_ = {dev.bufferAllocator(), sizeof(RenderParams),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

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
	vpp::Init<vpp::Image> initSelectionStage(dev, imgi, dev.hostMemoryTypes(),
		&batch.alloc.memHost);

	selectionCb_ = dev.commandAllocator().get(qfam,
		vk::CommandPoolCreateBits::resetCommandBuffer);
	selectionSemaphore_ = {dev};

	// TODO: we can't assume that timestamps are supported...
	timestampBits_ = dev.queueSubmitter().queue().properties().timestampValidBits;
	dlg_assert(timestampBits_);
	timestampBits_ = 0xFFFFFFFFu; // guaranteed

	// query pool
	vk::QueryPoolCreateInfo qpi;
	qpi.queryCount = timestampCount + 1;
	qpi.queryType = vk::QueryType::timestamp;
	queryPool_ = {dev, qpi};

	// hdr skybox
	// skybox_.init(dev, "../assets/kloofendal2k.hdr", fwd_.pass, 0u, samples());
	skybox_.init(batch, "/home/nyorain/art/assets/apt2k.hdr", fwd_.pass, 0u,
		samples());

	vpp::Init<vpp::SubBuffer> initBoxIndices(alloc.bufDevice,
		36u * sizeof(std::uint16_t),
		vk::BufferUsageBits::indexBuffer | vk::BufferUsageBits::transferDst,
		dev.deviceMemoryTypes(), 4u);

	// Load Model
	auto s = sceneScale_;
	auto mat = doi::scaleMat<4, float>({s, s, s});
	auto samplerAnisotropy = 1.f;
	if(anisotropy_) {
		samplerAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
		samplerAnisotropy = std::max(samplerAnisotropy, maxAnisotropy_);
	}

	auto ri = doi::SceneRenderInfo{
		materialDsLayout_,
		primitiveDsLayout_,
		dummyTex_.vkImageView(),
		samplerAnisotropy
	};

	auto [omodel, path] = doi::loadGltf(modelname_);
	if(!omodel) {
		std::exit(-1);
	}

	auto model = std::move(*omodel);
	dlg_info("Found {} scenes", model.scenes.size());
	auto scene = model.defaultScene >= 0 ? model.defaultScene : 0;
	auto& sc = model.scenes[scene];
	vpp::Init<doi::Scene> initScene(batch, path, *omodel, sc,
		mat, ri);

	// allocate
	scene_ = initScene.init(batch);
	boxIndices_ = initBoxIndices.init();
	selectionStage_ = initSelectionStage.init();
	sceneDs_ = initSceneDs.init();
	sceneUbo_ = initSceneUbo.init();

	// cb
	std::array<std::uint16_t, 36> indices = {
		0, 1, 2,  2, 1, 3, // front
		1, 5, 3,  3, 5, 7, // right
		2, 3, 6,  6, 3, 7, // top
		4, 0, 6,  6, 0, 2, // left
		4, 5, 0,  0, 5, 1, // bottom
		5, 4, 7,  7, 4, 6, // back
	};

	auto boxIndicesStage = vpp::fillStaging(cb, boxIndices_, indices);

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

	// submit and wait
	// NOTE: we could do something on the cpu meanwhile
	vk::endCommandBuffer(cb);
	auto id = qs.add(cb);
	qs.wait(id);

	// scene descriptor, used for some pipelines as set 0 for camera
	// matrix and view position
	vpp::DescriptorSetUpdate sdsu(sceneDs_);
	sdsu.uniform({{{sceneUbo_}}});
	sdsu.apply();

	currentID_ = scene_.primitives().size();
	lightMaterial_.emplace(materialDsLayout_,
		dummyTex_.vkImageView(), scene_.defaultSampler(),
		nytl::Vec{0.f, 0.f, 0.0f, 0.f}, 0.f, 0.f, false,
		nytl::Vec{1.f, 0.9f, 0.5f});

	// TODO: defer as well
	if(pointLight) {
		auto& l = pointLights_.emplace_back(dev, lightDsLayout_,
			primitiveDsLayout_, shadowData_, currentID_++);
		l.data.position = {-1.8f, 6.0f, -2.f};
		l.data.color = {2.f, 1.7f, 0.8f};
		l.data.attenuation = {1.f, 0.3f, 0.1f};
		l.data.radius = 10.f;
		l.updateDevice();
		// pp_.params.scatterLightColor = 0.1f * l.data.color;
	} else {
		auto& l = dirLights_.emplace_back(dev, lightDsLayout_,
			primitiveDsLayout_, shadowData_, camera_.pos, currentID_++);
		l.data.dir = {-3.8f, -9.2f, -5.2f};
		l.data.color = {2.f, 1.7f, 0.8f};
		l.updateDevice(camera_.pos);
		// pp_.params.scatterLightColor = 0.05f * l.data.color;
	}
}

void ViewApp::initGPass() {
	auto& dev = vulkanDevice();

	// render pass
	std::array<vk::AttachmentDescription, 5> attachments;
	struct {
		unsigned normals = 0;
		unsigned albedo = 1;
		unsigned emission = 2;
		unsigned depth = 3;
		unsigned ldepth = 4;
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
	addGBuf(ids.ldepth, ldepthFormat);

	// subpass
	vk::AttachmentReference gbufRefs[4];
	gbufRefs[0].attachment = ids.normals;
	gbufRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[1].attachment = ids.albedo;
	gbufRefs[1].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[2].attachment = ids.emission;
	gbufRefs[2].layout = vk::ImageLayout::colorAttachmentOptimal;
	gbufRefs[3].attachment = ids.ldepth;
	gbufRefs[3].layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference depthRef;
	depthRef.attachment = ids.depth;
	depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 4;
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
		vk::PipelineStageBits::lateFragmentTests |
		vk::PipelineStageBits::transfer;
	dependency.dstAccessMask = vk::AccessBits::inputAttachmentRead |
		vk::AccessBits::depthStencilAttachmentRead |
		vk::AccessBits::depthStencilAttachmentWrite |
		vk::AccessBits::transferRead |
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
	auto stages =  vk::ShaderStageBits::vertex |
		vk::ShaderStageBits::fragment |
		vk::ShaderStageBits::compute;
	auto sceneBindings = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer, stages)
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

	gpass_.pipeLayout = {dev, {{
		sceneDsLayout_.vkHandle(),
		materialDsLayout_.vkHandle(),
		primitiveDsLayout_.vkHandle(),
	}}, {{pcr}}};

	vpp::ShaderModule vertShader(dev, deferred_gbuf_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_gbuf_frag_data);
	vpp::GraphicsPipelineInfo gpi {gpass_.pass, gpass_.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}, 0};

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
	auto blendAttachments = {
		noBlendAttachment(),
		noBlendAttachment(),
		noBlendAttachment(),
		noBlendAttachment()
	};

	gpi.blend.attachmentCount = blendAttachments.size();
	gpi.blend.pAttachments = blendAttachments.begin();

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
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), nullptr, vkpipe);

	gpass_.pipe = {dev, vkpipe};
}

void ViewApp::initLPass() {
	auto& dev = vulkanDevice();

	std::array<vk::AttachmentDescription, 5> attachments;
	struct {
		unsigned normals = 0;
		unsigned albedo = 1;
		unsigned emission = 2;
		unsigned depth = 3; // TODO: use ldepth here?
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
	gbufRefs[2].layout = vk::ImageLayout::general;
	gbufRefs[3].attachment = ids.depth;
	gbufRefs[3].layout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::AttachmentReference outputRefs[2];
	outputRefs[0].attachment = ids.light;
	outputRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;
	outputRefs[1].attachment = ids.emission;
	outputRefs[1].layout = vk::ImageLayout::general;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 2u;
	subpass.pColorAttachments = outputRefs;
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
		vk::AccessBits::shaderRead |
		vk::AccessBits::shaderWrite;

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

	lpass_.pipeLayout = {dev, {{
		sceneDsLayout_.vkHandle(),
		lpass_.dsLayout.vkHandle(),
		lightDsLayout_.vkHandle()
	}}, {}};

	vpp::ShaderModule pointVertShader(dev, deferred_pointLight_vert_data);
	vpp::ShaderModule pointFragShader(dev, deferred_pointLight_frag_data);
	vpp::GraphicsPipelineInfo pgpi{lpass_.pass, lpass_.pipeLayout, {{{
		{pointVertShader, vk::ShaderStageBits::vertex},
		{pointFragShader, vk::ShaderStageBits::fragment},
	}}}};

	// TODO: enable depth test for additional discarding by rasterizer
	// (better performance i guess). requires depth attachment in this pass
	// though. Don't enable depth write!
	pgpi.depthStencil.depthTestEnable = false;
	pgpi.depthStencil.depthWriteEnable = false;
	pgpi.assembly.topology = vk::PrimitiveTopology::triangleList;
	// TODO: why does it only work with front?
	pgpi.rasterization.cullMode = vk::CullModeBits::front;
	pgpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

	// additive blending
	vk::PipelineColorBlendAttachmentState blendAttachments[2];
	blendAttachments[0].blendEnable = true;
	blendAttachments[0].colorBlendOp = vk::BlendOp::add;
	blendAttachments[0].srcColorBlendFactor = vk::BlendFactor::one;
	blendAttachments[0].dstColorBlendFactor = vk::BlendFactor::one;
	blendAttachments[0].alphaBlendOp = vk::BlendOp::add;
	blendAttachments[0].srcAlphaBlendFactor = vk::BlendFactor::one;
	blendAttachments[0].dstAlphaBlendFactor = vk::BlendFactor::one;
	blendAttachments[0].colorWriteMask =
		vk::ColorComponentBits::r |
		vk::ColorComponentBits::g |
		vk::ColorComponentBits::b |
		vk::ColorComponentBits::a;
	blendAttachments[1] = blendAttachments[0];

	pgpi.blend.attachmentCount = 2u;
	pgpi.blend.pAttachments = blendAttachments;
	pgpi.flags(vk::PipelineCreateBits::allowDerivatives);

	// dir light
	// here we can use a fullscreen shader pass since directional lights
	// don't have a light volume
	// TODO: don't load fullscreen shader multiple times
	vpp::ShaderModule dirFragShader(dev, deferred_dirLight_frag_data);
	vpp::ShaderModule fullVertShader(dev, stage_fullscreen_vert_data);
	vpp::GraphicsPipelineInfo lgpi{lpass_.pass, lpass_.pipeLayout, {{{
		{fullVertShader, vk::ShaderStageBits::vertex},
		{dirFragShader, vk::ShaderStageBits::fragment},
	}}}};

	lgpi.base(0); // base index
	lgpi.blend = pgpi.blend;
	lgpi.depthStencil = pgpi.depthStencil;
	lgpi.assembly = pgpi.assembly;

	// create the pipes
	vk::GraphicsPipelineCreateInfo infos[] = {pgpi.info(), lgpi.info()};
	vk::Pipeline vkpipes[2];
	vk::createGraphicsPipelines(dev, {}, 2, *infos, nullptr, *vkpipes);

	lpass_.pointPipe = {dev, vkpipes[0]};
	lpass_.dirPipe = {dev, vkpipes[1]};
}
void ViewApp::initPPass() {
	auto& dev = vulkanDevice();

	// render pass
	vk::AttachmentDescription attachment;
	attachment.format = swapchainInfo().imageFormat;
	attachment.samples = samples();
	attachment.loadOp = vk::AttachmentLoadOp::dontCare;
	attachment.storeOp = vk::AttachmentStoreOp::store;
	attachment.stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachment.stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachment.initialLayout = vk::ImageLayout::undefined;
	attachment.finalLayout = vk::ImageLayout::presentSrcKHR;

	// subpass
	vk::AttachmentReference colorRef;
	colorRef.attachment = 0u;
	colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = 1u;
	rpi.pAttachments = &attachment;
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	pp_.pass = {dev, rpi};

	// pipe
	auto ppInputBindings = {
		vpp::descriptorBinding( // params ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
		// we use the nearest sampler here since we use it for fxaa and ssr
		// and for ssr we *need* nearest (otherwise be bleed artefacts).
		// Not sure about fxaa but seems to work with nearest.
		vpp::descriptorBinding( // output from combine pass
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &nearestSampler_.vkHandle()),
		// we sample per pixel, nearest should be alright
		vpp::descriptorBinding( // ssr output
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &nearestSampler_.vkHandle()),
		// here it's important to use a linear sampler since we upscale
		vpp::descriptorBinding( // bloom
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &linearSampler_.vkHandle()),
		// depth (for ssr), use nearest sampler as ssr does
		vpp::descriptorBinding( // bloom
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &nearestSampler_.vkHandle()),
	};

	pp_.dsLayout = {dev, ppInputBindings};
	pp_.ds = {device().descriptorAllocator(), pp_.dsLayout};

	pp_.pipeLayout = {dev, {{pp_.dsLayout.vkHandle()}}, {}};

	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_pp_frag_data);
	vpp::GraphicsPipelineInfo gpi {pp_.pass, pp_.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}};

	gpi.depthStencil.depthTestEnable = false;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	gpi.blend.attachmentCount = 1u;
	gpi.blend.pAttachments = &noBlendAttachment();

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), nullptr, vkpipe);

	pp_.pipe = {dev, vkpipe};
}

ViewApp::SSAOInitData ViewApp::initSSAO(const doi::WorkBatcher& wb) {
	SSAOInitData initData;
	auto& dev = vulkanDevice();

	// render pass
	vk::AttachmentDescription attachment;
	attachment.format = ssaoFormat;
	attachment.samples = vk::SampleCountBits::e1;
	attachment.loadOp = vk::AttachmentLoadOp::dontCare;
	attachment.storeOp = vk::AttachmentStoreOp::store;
	attachment.stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachment.stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachment.initialLayout = vk::ImageLayout::undefined;
	attachment.finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::AttachmentReference colorRef;
	colorRef.attachment = 0u;
	colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;

	// TODO: not sure if correct/the most efficient
	vk::SubpassDependency dependency;
	dependency.srcSubpass = 0u;
	dependency.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
	dependency.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
	dependency.dstSubpass = vk::subpassExternal;
	dependency.dstStageMask = vk::PipelineStageBits::fragmentShader |
		vk::PipelineStageBits::computeShader;
	dependency.dstAccessMask = vk::AccessBits::shaderRead;

	vk::RenderPassCreateInfo rpi;
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;
	rpi.attachmentCount = 1u;
	rpi.pAttachments = &attachment;
	rpi.dependencyCount = 1u;
	rpi.pDependencies = &dependency;

	ssao_.pass = {dev, rpi};

	// pipeline
	auto ssaoBindings = {
		vpp::descriptorBinding( // ssao samples and such
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // ssao noise texture
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding( // depth texture
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &linearSampler_.vkHandle()),
		vpp::descriptorBinding( // normals texture
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &nearestSampler_.vkHandle()),
	};

	ssao_.dsLayout = {dev, ssaoBindings};
	ssao_.pipeLayout = {dev, {{
		sceneDsLayout_.vkHandle(),
		ssao_.dsLayout.vkHandle()
	}}, {}};

	vk::SpecializationMapEntry entry;
	entry.constantID = 0u;
	entry.offset = 0u;

	auto data = std::uint32_t(ssaoSampleCount);
	vk::SpecializationInfo spi;
	spi.mapEntryCount = 1;
	spi.pMapEntries = &entry;
	spi.dataSize = sizeof(std::uint32_t);
	spi.pData = &data;

	// TODO: fullscreen shader used by multiple passes, don't reload
	// it every time...
	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_ssao_frag_data);
	vpp::GraphicsPipelineInfo gpi {ssao_.pass, ssao_.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment, &spi},
	}}}};

	gpi.depthStencil.depthTestEnable = false;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	gpi.blend.attachmentCount = 1u;
	gpi.blend.pAttachments = &noBlendAttachment();

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), nullptr, vkpipe);

	ssao_.pipe = {dev, vkpipe};

	// ssao
	std::default_random_engine rndEngine;
	std::uniform_real_distribution<float> rndDist(0.0f, 1.0f);

	// Sample kernel
	std::vector<nytl::Vec4f> ssaoKernel(ssaoSampleCount);
	for(auto i = 0u; i < ssaoSampleCount; ++i) {
		nytl::Vec3f sample{
			2.f * rndDist(rndEngine) - 1.f,
			2.f * rndDist(rndEngine) - 1.f,
			rndDist(rndEngine)};
		sample = normalized(sample);
		sample *= rndDist(rndEngine);
		float scale = float(i) / float(ssaoSampleCount);
		scale = nytl::mix(0.1f, 1.0f, scale * scale);
		ssaoKernel[i] = nytl::Vec4f(scale * sample);
	}

	// sort for better cache coherency on the gpu
	// not sure if that really effects something but it shouldn't hurt
	auto sorter = [](auto& vec1, auto& vec2) {
		return std::atan2(vec1[1], vec1[0]) < std::atan2(vec2[1], vec2[0]);
	};
	std::sort(ssaoKernel.begin(), ssaoKernel.end(), sorter);

	// ubo
	auto devMem = dev.deviceMemoryTypes();
	auto size = sizeof(nytl::Vec4f) * ssaoSampleCount;
	auto usage = vk::BufferUsageBits::transferDst |
		vk::BufferUsageBits::uniformBuffer;
	ssao_.samples = {dev.bufferAllocator(), size, usage, devMem};
	auto samplesSpan = nytl::span(ssaoKernel);
	initData.samplesStage = vpp::fillStaging(wb.cb, ssao_.samples,
		as_bytes(samplesSpan));

	// NOTE: we could use a r32g32f format, would be more efficent
	// might not be supported though... we could pack it into somehow
	auto noiseDim = 4u;
	auto noiseSize = sizeof(nytl::Vec4f) * noiseDim * noiseDim;
	auto noiseData = std::make_unique<std::byte[]>(noiseSize);
	auto noise = reinterpret_cast<nytl::Vec4f*>(noiseData.get());
	for(auto i = 0u; i < noiseDim * noiseDim; i++) {
		noise[i] = nytl::Vec4f{
			rndDist(rndEngine) * 2.f - 1.f,
			rndDist(rndEngine) * 2.f - 1.f,
			0.0f, 0.0f
		};
	}

	std::vector<std::unique_ptr<std::byte[]>> layers;
	layers.emplace_back(std::move(noiseData));

	// TODO: defer
	auto params = doi::TextureCreateParams{};
	params.format = vk::Format::r32g32b32a32Sfloat;
	ssao_.noise = {wb, std::move(layers), vk::Format::r32g32b32a32Sfloat,
		{noiseDim, noiseDim}, params};

	ssao_.ds = {dev.descriptorAllocator(), ssao_.dsLayout};
	return initData;
}

void ViewApp::initSSAOBlur() {
	auto& dev = vulkanDevice();

	// render pass
	// layouts
	auto ssaoBlurBindings = {
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &nearestSampler_.vkHandle()),
	};
	ssao_.blur.dsLayout = {dev, ssaoBlurBindings};
	ssao_.blur.dsHorz = {dev.descriptorAllocator(), ssao_.blur.dsLayout};
	ssao_.blur.dsVert = {dev.descriptorAllocator(), ssao_.blur.dsLayout};

	vk::PushConstantRange pcr;
	pcr.size = 4;
	pcr.stageFlags = vk::ShaderStageBits::fragment;
	ssao_.blur.pipeLayout = {dev, {{ssao_.blur.dsLayout.vkHandle()}}, {{pcr}}};

	// pipe
	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_ssaoBlur_frag_data);
	vpp::GraphicsPipelineInfo gpi {ssao_.pass, ssao_.blur.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}};

	gpi.depthStencil.depthTestEnable = false;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	gpi.blend.attachmentCount = 1u;
	gpi.blend.pAttachments = &noBlendAttachment();

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), nullptr, vkpipe);

	ssao_.blur.pipe = {dev, vkpipe};
}

void ViewApp::initScattering() {
	// render pass
	auto& dev = device();
	std::array<vk::AttachmentDescription, 1u> attachments;

	// light scattering output format
	attachments[0].format = scatterFormat;
	attachments[0].samples = vk::SampleCountBits::e1;
	attachments[0].loadOp = vk::AttachmentLoadOp::dontCare;
	attachments[0].storeOp = vk::AttachmentStoreOp::store;
	attachments[0].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[0].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[0].initialLayout = vk::ImageLayout::undefined;
	attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	// subpass
	vk::AttachmentReference colorRef;
	colorRef.attachment = 0u;
	colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;

	// TODO: just as with ssao, need an additional barrier between
	// this and the consuming pass (in this case light or post processing)
	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	scatter_.pass = {dev, rpi};

	// pipeline
	auto scatterBindings = {
		vpp::descriptorBinding( // depthTex
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &linearSampler_.vkHandle()),
	};

	scatter_.dsLayout = {dev, scatterBindings};
	scatter_.pipeLayout = {dev, {{
		sceneDsLayout_.vkHandle(),
		scatter_.dsLayout.vkHandle(),
		lightDsLayout_.vkHandle()
	}}, {}};

	// TODO: at least for point light, we don't have to use a fullscreen
	// pass here! that really should bring quite the improvement (esp
	// if we later on allow multiple point light scattering effects)
	// TODO: fullscreen shader used by multiple passes, don't reload
	// it every time...
	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule pfragShader(dev, deferred_pointScatter_frag_data);
	vpp::GraphicsPipelineInfo pgpi{scatter_.pass, scatter_.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{pfragShader, vk::ShaderStageBits::fragment},
	}}}};

	pgpi.flags(vk::PipelineCreateBits::allowDerivatives);
	pgpi.depthStencil.depthTestEnable = false;
	pgpi.depthStencil.depthWriteEnable = false;
	pgpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	pgpi.blend.attachmentCount = 1u;
	pgpi.blend.pAttachments = &noBlendAttachment();

	// directionoal
	vpp::ShaderModule dfragShader(dev, deferred_dirScatter_frag_data);
	vpp::GraphicsPipelineInfo dgpi{scatter_.pass, scatter_.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{dfragShader, vk::ShaderStageBits::fragment},
	}}}};

	dgpi.depthStencil = pgpi.depthStencil;
	dgpi.assembly = pgpi.assembly;
	dgpi.blend = pgpi.blend;
	dgpi.base(0);

	vk::Pipeline vkpipes[2];
	auto infos = {pgpi.info(), dgpi.info()};
	vk::createGraphicsPipelines(dev, {}, infos.size(), *infos.begin(),
		nullptr, *vkpipes);

	scatter_.pointPipe = {dev, vkpipes[0]};
	scatter_.dirPipe = {dev, vkpipes[1]};
	scatter_.ds = {dev.descriptorAllocator(), scatter_.dsLayout};
}

void ViewApp::initBloom() {
	// layouts
	auto& dev = vulkanDevice();
	auto blurBindings = {
		// important to use linear sampler here, our shader is optimized,
		// reads multiple pixels in a single fetch via linear sampling
		vpp::descriptorBinding( // input color
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &linearSampler_.vkHandle()),
		vpp::descriptorBinding( // output color, blurred
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute)
	};

	vk::PushConstantRange pcr;
	pcr.size = 4;
	pcr.stageFlags = vk::ShaderStageBits::compute;

	bloom_.dsLayout = {dev, blurBindings};
	bloom_.pipeLayout = {dev, {{bloom_.dsLayout.vkHandle()}}, {{pcr}}};

	// pipe
	vpp::ShaderModule blurShader(dev, deferred_gblur_comp_data);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = bloom_.pipeLayout;
	cpi.stage.module = blurShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";

	vk::Pipeline vkpipe;
	vk::createComputePipelines(dev, {}, 1u, cpi, nullptr, vkpipe);
	bloom_.pipe = {dev, vkpipe};
}

void ViewApp::initSSR() {
	// layouts
	auto& dev = vulkanDevice();
	auto ssrBindings = {
		vpp::descriptorBinding( // linear depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding( // normals
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding( // output data
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute)
	};

	ssr_.dsLayout = {dev, ssrBindings};
	ssr_.ds = {dev.descriptorAllocator(), ssr_.dsLayout};
	ssr_.pipeLayout = {dev, {{
		sceneDsLayout_.vkHandle(),
		ssr_.dsLayout.vkHandle()
	}}, {}};

	// pipe
	vpp::ShaderModule ssrShader(dev, deferred_ssr_comp_data);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = ssr_.pipeLayout;
	cpi.stage.module = ssrShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";

	vk::Pipeline vkpipe;
	vk::createComputePipelines(dev, {}, 1u, cpi, nullptr, vkpipe);
	ssr_.pipe = {dev, vkpipe};
}

void ViewApp::initCombine() {
	// layouts
	auto& dev = vulkanDevice();
	auto combineBindings = {
		vpp::descriptorBinding( // target
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // albedo
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding( // ssao
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding( // emission
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding( // normal
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding( // linear depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &nearestSampler_.vkHandle()),
		vpp::descriptorBinding( // irradiance
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &linearSampler_.vkHandle()),
	};

	combine_.dsLayout = {dev, combineBindings};
	combine_.ds = {dev.descriptorAllocator(), combine_.dsLayout};
	combine_.pipeLayout = {dev, {{
		sceneDsLayout_.vkHandle(),
		combine_.dsLayout.vkHandle()
	}}, {}};

	// pipe
	vpp::ShaderModule combineShader(dev, deferred_combine_comp_data);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = combine_.pipeLayout;
	cpi.stage.module = combineShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";

	vk::Pipeline vkpipe;
	vk::createComputePipelines(dev, {}, 1u, cpi, nullptr, vkpipe);
	combine_.pipe = {dev, vkpipe};
}

void ViewApp::initDebug() {
	auto& dev = vulkanDevice();

	// layouts
	auto debugInputBindings = {
		vpp::descriptorBinding( // params ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // albedo
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // normal
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // depth
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // ssao
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // ssr
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // ssr
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &linearSampler_.vkHandle()),
	};

	debug_.dsLayout = {dev, debugInputBindings};
	debug_.ds = {device().descriptorAllocator(), debug_.dsLayout};

	debug_.pipeLayout = {dev, {{debug_.dsLayout.vkHandle()}}, {}};

	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_debug_frag_data);
	vpp::GraphicsPipelineInfo gpi {pp_.pass, debug_.pipeLayout, {{{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}}};

	gpi.depthStencil.depthTestEnable = false;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	gpi.blend.attachmentCount = 1u;
	gpi.blend.pAttachments = &noBlendAttachment();

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), nullptr, vkpipe);

	debug_.pipe = {dev, vkpipe};
}

void ViewApp::initFwd() {
	// render pass
	auto& dev = device();
	std::array<vk::AttachmentDescription, 2u> attachments;

	// light scattering output format
	attachments[0].format = lightFormat;
	attachments[0].samples = vk::SampleCountBits::e1;
	attachments[0].loadOp = vk::AttachmentLoadOp::load;
	attachments[0].storeOp = vk::AttachmentStoreOp::store;
	attachments[0].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[0].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[0].initialLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	attachments[1].format = depthFormat();
	attachments[1].samples = vk::SampleCountBits::e1;
	attachments[1].loadOp = vk::AttachmentLoadOp::load;
	attachments[1].storeOp = vk::AttachmentStoreOp::store;
	attachments[1].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[1].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[1].initialLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	attachments[1].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	// subpass
	vk::AttachmentReference colorRef;
	colorRef.attachment = 0u;
	colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference depthRef;
	depthRef.attachment = 1u;
	depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;
	subpass.pDepthStencilAttachment = &depthRef;

	// TODO: probably needs a barrier
	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	fwd_.pass = {dev, rpi};

	// TODO: init pipeline and stuff for transparent objects
	// currently only used for skybox
}

vpp::ViewableImage ViewApp::createDepthTarget(const vk::Extent2D& size) {
	auto width = size.width;
	auto height = size.height;

	// img
	vk::ImageCreateInfo img;
	img.imageType = vk::ImageType::e2d;
	img.format = depthFormat();
	img.extent.width = width;
	img.extent.height = height;
	img.extent.depth = 1;
	img.mipLevels = 1;
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
	vpp::ViewableImage target = {device(), img, view,
		device().deviceMemoryTypes()};

	return target;
}

static constexpr auto defaultAttachmentUsage =
	vk::ImageUsageBits::colorAttachment |
	vk::ImageUsageBits::inputAttachment |
	vk::ImageUsageBits::transferSrc |
	vk::ImageUsageBits::sampled;

void ViewApp::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	auto& dev = vulkanDevice();
	auto devMem = dev.deviceMemoryTypes();
	depthTarget() = createDepthTarget(size);

	std::vector<vk::ImageView> attachments;
	auto getCreateInfo = [&](vk::Format format,
			vk::ImageUsageFlags usage = defaultAttachmentUsage,
			vk::ImageAspectBits aspect = vk::ImageAspectBits::color) {
		auto info = vpp::ViewableImageCreateInfo(format,
			aspect, {size.width, size.height}, usage);
		dlg_assert(vpp::supported(dev, info.img));
		return info;
	};

	auto initBuf = [&](vpp::ViewableImage& img, vk::Format format,
			vk::ImageUsageFlags extraUsage = {}) {
		auto info = getCreateInfo(format,
			defaultAttachmentUsage | extraUsage);
		img = {device(), info, devMem};
		attachments.push_back(img.vkImageView());
	};

	// create offscreen buffers, gbufs
	initBuf(gpass_.normal, normalsFormat);
	initBuf(gpass_.albedo, albedoFormat);

	// emission buf - we need mipmap levels here for bloom later
	auto usage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::inputAttachment |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst |
		vk::ImageUsageBits::storage |
		vk::ImageUsageBits::sampled;
	auto info = getCreateInfo(emissionFormat, usage);
	auto bloomLevels = std::min(maxBloomLevels, vpp::mipmapLevels(size));
	info.img.mipLevels = 1 + bloomLevels;
	gpass_.emission = {device(), info, devMem};

	attachments.push_back(gpass_.emission.vkImageView());
	attachments.push_back(depthTarget().vkImageView()); // depth

	// TODO!
	// depthMipLevels_ = vpp::mipmapLevels(size);
	depthMipLevels_ = 1u;
	usage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::inputAttachment |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst |
		vk::ImageUsageBits::sampled;
	info = getCreateInfo(ldepthFormat, usage);
	info.img.mipLevels = depthMipLevels_;
	gpass_.depth = {device(), info, devMem};
	attachments.push_back(gpass_.depth.vkImageView());

	info.view.subresourceRange.levelCount = depthMipLevels_;
	info.view.image = gpass_.depth.image();
	depthMipView_ = {device(), info.view};

	vk::FramebufferCreateInfo fbi({}, gpass_.pass,
		attachments.size(), attachments.data(),
		size.width, size.height, 1);
	gpass_.fb = {dev, fbi};

	// light buf
	attachments.clear();
	attachments.push_back(gpass_.normal.vkImageView());
	attachments.push_back(gpass_.albedo.vkImageView());
	attachments.push_back(gpass_.emission.vkImageView());
	attachments.push_back(depthTarget().vkImageView());

	initBuf(lpass_.light, lightFormat, vk::ImageUsageBits::storage);
	fbi = vk::FramebufferCreateInfo({}, lpass_.pass,
		attachments.size(), attachments.data(),
		size.width, size.height, 1);
	lpass_.fb = {dev, fbi};

	// ssao buf
	if(params_.flags & flagSSAO) {
		attachments.clear();
		initBuf(ssao_.target, ssaoFormat);
		fbi = vk::FramebufferCreateInfo({}, ssao_.pass,
			attachments.size(), attachments.data(),
			size.width, size.height, 1);
		ssao_.fb = {dev, fbi};

		// ssao blur
		attachments.clear();
		initBuf(ssao_.blur.target, ssaoFormat);
		fbi = vk::FramebufferCreateInfo({}, ssao_.pass,
			attachments.size(), attachments.data(),
			size.width, size.height, 1);
		ssao_.blur.fb = {dev, fbi};
	} else {
		ssao_.blur.fb = {};
		ssao_.target = {};
		ssao_.blur.target = {};
		ssao_.blur.fb = {};
	}

	// scatter buf
	if(params_.flags & flagScattering) {
		attachments.clear();
		initBuf(scatter_.target, scatterFormat);
		fbi = vk::FramebufferCreateInfo({}, scatter_.pass,
			attachments.size(), attachments.data(),
			size.width, size.height, 1);
		scatter_.fb = {dev, fbi};
	} else {
		scatter_.target = {};
		scatter_.fb = {};
	}

	// TODO: really start with half-sized bloom for first level?
	// try out different settings/make it confiurable. Should have
	// major impact on visuals *and* performance
	if(params_.flags & flagBloom) {
		usage = vk::ImageUsageBits::storage | vk::ImageUsageBits::sampled;
		info = getCreateInfo(emissionFormat, usage);
		info.img.extent = {size.width / 2, size.height / 2, 1u};
		info.img.mipLevels = bloomLevels;
		bloom_.tmpTarget = {device(), info.img, devMem};

		bloomLevels_ = bloomLevels;
		bloom_.emissionLevels.clear();
		bloom_.tmpLevels.clear();
		for(auto i = 0u; i < bloomLevels; ++i) {
			auto ivi = info.view;

			// emission view
			auto& emissionLevel = bloom_.emissionLevels.emplace_back();
			ivi.subresourceRange.baseMipLevel = i + 1;
			ivi.image = gpass_.emission.image();
			emissionLevel.view = {dev, ivi};
			emissionLevel.ds = {dev.descriptorAllocator(), bloom_.dsLayout};

			// tmp target
			auto& tmpLevel = bloom_.tmpLevels.emplace_back();
			ivi.image = bloom_.tmpTarget;
			ivi.subresourceRange.baseMipLevel = i;
			tmpLevel.view = {dev, ivi};
			tmpLevel.ds = {dev.descriptorAllocator(), bloom_.dsLayout};
		}

		auto ivi = info.view;
		ivi.image = gpass_.emission.image();
		ivi.subresourceRange.baseMipLevel = 0;
		ivi.subresourceRange.levelCount = bloomLevels;
		bloom_.fullBloom = {dev, ivi};
	} else {
		bloom_.tmpLevels.clear();
		bloom_.fullBloom = {};
		bloom_.tmpTarget = {};
	}

	// ssr target image
	if(params_.flags & flagSSR) {
		usage = vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::inputAttachment;
		info = getCreateInfo(ssrFormat, usage);
		ssr_.target = {device(), info, devMem};
	} else {
		ssr_.target = {};
	}

	// fwd
	attachments.clear();
	attachments.push_back(lpass_.light.vkImageView());
	attachments.push_back(depthTarget().vkImageView());
	fbi = vk::FramebufferCreateInfo({}, fwd_.pass,
		attachments.size(), attachments.data(),
		size.width, size.height, 1);
	fwd_.fb = {dev, fbi};

	// create swapchain framebuffers
	for(auto& buf : bufs) {
		attachments.clear();
		attachments.push_back(buf.imageView);
		vk::FramebufferCreateInfo fbi({}, pp_.pass,
			attachments.size(), attachments.data(),
			size.width, size.height, 1);
		buf.framebuffer = {dev, fbi};
	}

	updateDescriptors();
}

void ViewApp::updateDescriptors() {
	updateDescriptors_ = false;
	std::vector<vpp::DescriptorSetUpdate> updates;

	auto& ldsu = updates.emplace_back(lpass_.ds);
	ldsu.inputAttachment({{{}, gpass_.normal.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.inputAttachment({{{}, gpass_.albedo.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.inputAttachment({{{}, gpass_.emission.vkImageView(),
		vk::ImageLayout::general}});
	ldsu.inputAttachment({{{}, depthTarget().imageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});

	bool needDepth = (params_.flags & flagSSR) || (params_.flags & flagSSAO);
	auto ssaoView = (params_.flags & flagSSAO) ?
		ssao_.target.vkImageView() : dummyTex_.vkImageView();
	// auto scatterView = (pp_.params.flags & flagScattering) ?
		// scatter_.target.vkImageView() : dummyTex_.vkImageView();
	auto depthView = needDepth ?  depthMipView_ : gpass_.depth.vkImageView();
	auto bloomView = (params_.flags & flagBloom) ?
		bloom_.fullBloom : gpass_.emission.vkImageView();
	auto ssrView = (params_.flags & flagSSR) ?
		ssr_.target.vkImageView() : dummyTex_.vkImageView();

	auto& pdsu = updates.emplace_back(pp_.ds);
	pdsu.uniform({{{paramsUbo_}}});
	pdsu.imageSampler({{{}, lpass_.light.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, ssrView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, bloomView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, depthView,
		vk::ImageLayout::shaderReadOnlyOptimal}});

	auto& cdsu = updates.emplace_back(combine_.ds);
	cdsu.storage({{{}, lpass_.light.vkImageView(),
		vk::ImageLayout::general}});
	cdsu.uniform({{{paramsUbo_}}});
	cdsu.imageSampler({{{}, gpass_.albedo.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	cdsu.imageSampler({{{}, ssaoView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	cdsu.imageSampler({{{}, bloomView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	cdsu.imageSampler({{{}, gpass_.normal.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	cdsu.imageSampler({{{}, depthView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	cdsu.imageSampler({{{}, skybox_.irradiance().vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});

	if(params_.flags & flagSSAO) {
		auto& ssdsu = updates.emplace_back(ssao_.ds);
		ssdsu.uniform({{{ssao_.samples}}});
		ssdsu.imageSampler({{{}, ssao_.noise.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ssdsu.imageSampler({{{}, depthView,
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ssdsu.imageSampler({{{}, gpass_.normal.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		auto& ssbhdsu = updates.emplace_back(ssao_.blur.dsHorz);
		ssbhdsu.imageSampler({{{}, ssao_.target.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ssbhdsu.imageSampler({{{}, depthView,
			vk::ImageLayout::shaderReadOnlyOptimal}});

		auto& ssbvdsu = updates.emplace_back(ssao_.blur.dsVert);
		ssbvdsu.imageSampler({{{}, ssao_.blur.target.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ssbvdsu.imageSampler({{{}, depthView,
			vk::ImageLayout::shaderReadOnlyOptimal}});
	}

	if(params_.flags & flagScattering) {
		auto& sdsu = updates.emplace_back(scatter_.ds);
		sdsu.imageSampler({{{}, depthView,
			vk::ImageLayout::shaderReadOnlyOptimal}});
	}

	if(params_.flags & flagBloom) {
		for(auto i = 0u; i < bloomLevels_; ++i) {
			auto& tmp = bloom_.tmpLevels[i];
			auto& emission = bloom_.emissionLevels[i];

			auto& bhdsu = updates.emplace_back(tmp.ds);
			bhdsu.imageSampler({{{}, emission.view,
				vk::ImageLayout::shaderReadOnlyOptimal}});
			bhdsu.storage({{{}, tmp.view, vk::ImageLayout::general}});

			auto& bvdsu = updates.emplace_back(emission.ds);
			bvdsu.imageSampler({{{}, tmp.view,
				vk::ImageLayout::shaderReadOnlyOptimal}});
			bvdsu.storage({{{}, emission.view, vk::ImageLayout::general}});
		}
	}

	if(params_.flags & flagSSR) {
		auto& rdsu = updates.emplace_back(ssr_.ds);
		rdsu.imageSampler({{{}, depthView,
			vk::ImageLayout::shaderReadOnlyOptimal}});
		rdsu.imageSampler({{{}, gpass_.normal.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		rdsu.storage({{{}, ssr_.target.vkImageView(),
			vk::ImageLayout::general}});
	}

	if(params_.mode != 0) {
		auto& ddsu = updates.emplace_back(debug_.ds);
		ddsu.uniform({{{paramsUbo_}}});
		ddsu.inputAttachment({{{}, gpass_.albedo.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ddsu.inputAttachment({{{}, gpass_.normal.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ddsu.inputAttachment({{{}, gpass_.depth.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ddsu.inputAttachment({{{}, ssaoView,
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ddsu.inputAttachment({{{}, ssrView,
			vk::ImageLayout::shaderReadOnlyOptimal}});
		ddsu.imageSampler({{{}, bloomView,
			vk::ImageLayout::shaderReadOnlyOptimal}});
	}

	vpp::apply(updates);
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
	defs.push_back({
		"maxAniso", {"--maxaniso", "--ani"},
		"Maximum of anisotropy for samplers", 1
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
	if(result.has_option("maxAniso")) {
		maxAnisotropy_ = result["maxAniso"].as<float>();
	}

	return true;
}

void ViewApp::record(const RenderBuffer& buf) {
	auto cb = buf.commandBuffer;
	vk::beginCommandBuffer(cb, {});
	App::beforeRender(cb);

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::topOfPipe,
		queryPool_, 0u);

	// render shadow maps
	for(auto& light : pointLights_) {
		if(light.hasShadowMap()) {
			light.render(cb, shadowData_, scene_);
		}
	}
	for(auto& light : dirLights_) {
		light.render(cb, shadowData_, scene_);
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 1u);

	const auto width = swapchainInfo().imageExtent.width;
	const auto height = swapchainInfo().imageExtent.height;

	// gpass
	{
		std::array<vk::ClearValue, 5u> cv {};
		cv[0] = {-1.f, -1.f, -1.f, -1.f}; // normals, snorm
		cv[1] = cv[2] = {{0.f, 0.f, 0.f, 0.f}};
		cv[3].depthStencil = {1.f, 0u};
		cv[4] = {{1000.f, 0.f, 0.f, 0.f}}; // r16f depth
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
			gpass_.pipeLayout, 0, {{sceneDs_.vkHandle()}}, {});
		scene_.render(cb, gpass_.pipeLayout);

		/*
		// NOTE: ideally, don't render these in gbuffer pass but later
		// on with different lightning?
		lightMaterial_->bind(cb, gpass_.pipeLayout);
		for(auto& l : pointLights_) {
			l.lightBall().render(cb, gpass_.pipeLayout);
		}
		for(auto& l : dirLights_) {
			l.lightBall().render(cb, gpass_.pipeLayout);
		}
		*/

		vk::cmdEndRenderPass(cb);
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 2u);

	// TODO: add back in if needed
	// create depth mipmaps
	/*
	bool needDepth = (params_.flags & flagSSR) || (params_.flags & flagSSAO);
	if(depthMipLevels_ > 1 && needDepth) {
		auto& depthImg = gpass_.depth.image();
		// transition mipmap level 0 transferSrc
		vpp::changeLayout(cb, depthImg,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::allGraphics,
			vk::AccessBits::depthStencilAttachmentWrite,
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

		// transition all but mipmap level 0 to transferDst
		vpp::changeLayout(cb, depthImg,
			vk::ImageLayout::undefined, // we don't need the content any more
			vk::PipelineStageBits::topOfPipe, {},
			vk::ImageLayout::transferDstOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferWrite,
			{vk::ImageAspectBits::color, 1, depthMipLevels_ - 1, 0, 1});

		for(auto i = 1u; i < depthMipLevels_; ++i) {
			// std::max needed for end offsets when the texture is not
			// quadratic: then we would get 0 there although the mipmap
			// still has size 1
			vk::ImageBlit blit;
			blit.srcSubresource.layerCount = 1;
			blit.srcSubresource.mipLevel = i - 1;
			blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.srcOffsets[1].x = std::max(width >> (i - 1), 1u);
			blit.srcOffsets[1].y = std::max(height >> (i - 1), 1u);
			blit.srcOffsets[1].z = 1u;

			blit.dstSubresource.layerCount = 1;
			blit.dstSubresource.mipLevel = i;
			blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.dstOffsets[1].x = std::max(width >> i, 1u);
			blit.dstOffsets[1].y = std::max(height >> i, 1u);
			blit.dstOffsets[1].z = 1u;

			vk::cmdBlitImage(cb, depthImg, vk::ImageLayout::transferSrcOptimal,
				depthImg, vk::ImageLayout::transferDstOptimal, {{blit}},
				vk::Filter::linear);

			// change layout of current mip level to transferSrc for next mip level
			vpp::changeLayout(cb, depthImg,
				vk::ImageLayout::transferDstOptimal,
				vk::PipelineStageBits::transfer,
				vk::AccessBits::transferWrite,
				vk::ImageLayout::transferSrcOptimal,
				vk::PipelineStageBits::transfer,
				vk::AccessBits::transferRead,
				{vk::ImageAspectBits::color, i, 1, 0, 1});
		}

		// transform all levels back to readonly
		vpp::changeLayout(cb, depthImg,
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::allCommands,
			vk::AccessBits::shaderRead,
			{vk::ImageAspectBits::color, 0, depthMipLevels_, 0, 1});
	}
	*/

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 3u);

	// ssao
	if(params_.flags & flagSSAO) {
		vk::cmdBeginRenderPass(cb, {
			ssao_.pass,
			ssao_.fb,
			{0u, 0u, width, height},
			0, nullptr
		}, {});

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics,
			ssao_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			ssao_.pipeLayout, 0,
			{{sceneDs_.vkHandle(), ssao_.ds.vkHandle()}}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);
	}

	// ssao blur
	if(params_.flags & flagSSAO) {
		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics,
			ssao_.blur.pipe);

		// horizontal
		vk::cmdBeginRenderPass(cb, {
			ssao_.pass,
			ssao_.blur.fb,
			{0u, 0u, width, height},
			0, nullptr
		}, {});

		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		std::uint32_t horizontal = 1u;
		vk::cmdPushConstants(cb, ssao_.blur.pipeLayout,
			vk::ShaderStageBits::fragment, 0, 4, &horizontal);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			ssao_.blur.pipeLayout, 0, {{ssao_.blur.dsHorz.vkHandle()}}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);

		// vertical
		vk::cmdBeginRenderPass(cb, {
			ssao_.pass,
			ssao_.fb,
			{0u, 0u, width, height},
			0, nullptr
		}, {});

		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		horizontal = 0u;
		vk::cmdPushConstants(cb, ssao_.blur.pipeLayout,
			vk::ShaderStageBits::fragment, 0, 4, &horizontal);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			ssao_.blur.pipeLayout, 0, {{ssao_.blur.dsVert.vkHandle()}}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 4u);

	// scatter
	if(params_.flags & flagScattering) {
		vk::cmdBeginRenderPass(cb, {
			scatter_.pass,
			scatter_.fb,
			{0u, 0u, width, height},
			0, nullptr
		}, {});

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		// TODO: scatter support for multiple lights
		vk::DescriptorSet lds;
		if(!dirLights_.empty()) {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics,
				scatter_.dirPipe);
			lds = dirLights_[0].ds();
		} else {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics,
				scatter_.pointPipe);
			lds = pointLights_[0].ds();
		}

		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			scatter_.pipeLayout, 0, {{sceneDs_.vkHandle(),
			scatter_.ds.vkHandle(), lds}}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 5u);

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

		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			lpass_.pipeLayout, 0,
			{{sceneDs_.vkHandle(), lpass_.ds.vkHandle()}}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, lpass_.pointPipe);
		vk::cmdBindIndexBuffer(cb, boxIndices_.buffer(),
			boxIndices_.offset(), vk::IndexType::uint16);
		for(auto& light : pointLights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				lpass_.pipeLayout, 2, {{light.ds().vkHandle()}}, {});
			vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0); // box
		}

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, lpass_.dirPipe);
		for(auto& light : dirLights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				lpass_.pipeLayout, 2, {{light.ds().vkHandle()}}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		}

		vk::cmdEndRenderPass(cb);
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 6u);

	// fwd
	{
		vk::cmdBeginRenderPass(cb, {
			fwd_.pass,
			fwd_.fb,
			{0u, 0u, width, height},
			0, nullptr,
		}, {});

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});
		vk::cmdBindIndexBuffer(cb, boxIndices_.buffer(),
			boxIndices_.offset(), vk::IndexType::uint16);
		skybox_.render(cb);
		vk::cmdEndRenderPass(cb);
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 7u);

	// bloom
	if(params_.flags & flagBloom) {
		// create emission mipmaps
		auto& emission = gpass_.emission.image();
		auto bloomLevels = bloomLevels_;

		// transition mipmap level 0 transferSrc
		vk::ImageMemoryBarrier barrier;
		barrier.image = emission;
		barrier.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.srcAccessMask =
			vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite;
		barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::allCommands,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});

		// transition all but mipmap level 0 to transferDst
		// bloomLevles is the number of bloom levels; without the original
		// (full sized) emission level
		barrier.subresourceRange.baseMipLevel = 1u;
		barrier.subresourceRange.levelCount = bloomLevels; // not -1
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.srcAccessMask = {};
		barrier.newLayout = vk::ImageLayout::transferDstOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferWrite;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});

		for(auto i = 1u; i < bloomLevels + 1; ++i) {
			// std::max needed for end offsets when the texture is not
			// quadratic: then we would get 0 there although the mipmap
			// still has size 1
			vk::ImageBlit blit;
			blit.srcSubresource.layerCount = 1;
			blit.srcSubresource.mipLevel = i - 1;
			blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.srcOffsets[1].x = std::max(width >> (i - 1), 1u);
			blit.srcOffsets[1].y = std::max(height >> (i - 1), 1u);
			blit.srcOffsets[1].z = 1u;

			blit.dstSubresource.layerCount = 1;
			blit.dstSubresource.mipLevel = i;
			blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.dstOffsets[1].x = std::max(width >> i, 1u);
			blit.dstOffsets[1].y = std::max(height >> i, 1u);
			blit.dstOffsets[1].z = 1u;

			vk::cmdBlitImage(cb, emission, vk::ImageLayout::transferSrcOptimal,
				emission, vk::ImageLayout::transferDstOptimal, {{blit}},
				vk::Filter::linear);

			// change layout of current mip level to transferSrc for next mip level
			barrier.subresourceRange.baseMipLevel = i;
			barrier.subresourceRange.levelCount = 1;
			barrier.oldLayout = vk::ImageLayout::transferDstOptimal;
			barrier.srcAccessMask = vk::AccessBits::transferWrite;
			barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
			barrier.dstAccessMask = vk::AccessBits::transferRead;
			vk::cmdPipelineBarrier(cb,
				vk::PipelineStageBits::transfer,
				vk::PipelineStageBits::transfer,
				{}, {}, {}, {{barrier}});
		}

		// transform all levels back to readonly for blur read pass
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = bloomLevels + 1;
		barrier.oldLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.srcAccessMask = vk::AccessBits::transferRead;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::allCommands,
			{}, {}, {}, {{barrier}});

		// make tmp target writable
		barrier.image = bloom_.tmpTarget;
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = bloomLevels;
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		barrier.newLayout = vk::ImageLayout::general;
		barrier.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::allCommands,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier}});

		// blur
		// we blur layer for layer (both passes every time) since that
		// might have cache advantages over first doing horizontal
		// for all and then doing vertical for all i guess
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, bloom_.pipe);
		for(auto i = 0u; i < bloomLevels; ++i) {
			auto& tmp = bloom_.tmpLevels[i];
			auto& emissionImg = emission;
			auto& emission = bloom_.emissionLevels[i];

			std::uint32_t horizontal = 1u;
			vk::cmdPushConstants(cb, bloom_.pipeLayout, vk::ShaderStageBits::compute,
				0, 4, &horizontal);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
				bloom_.pipeLayout, 0, {{tmp.ds.vkHandle()}}, {});

			auto w = std::max(width >> (i + 1), 1u);
			auto h = std::max(height >> (i + 1), 1u);
			vk::cmdDispatch(cb, std::ceil(w / 32.f), std::ceil(h / 32.f), 1);

			barrier.image = bloom_.tmpTarget;
			barrier.oldLayout = vk::ImageLayout::general;
			barrier.srcAccessMask = vk::AccessBits::shaderRead |
				vk::AccessBits::shaderWrite,
			barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
			barrier.dstAccessMask = vk::AccessBits::shaderRead;
			barrier.subresourceRange = {vk::ImageAspectBits::color, i, 1, 0, 1};
			vk::cmdPipelineBarrier(cb,
				vk::PipelineStageBits::computeShader |
					vk::PipelineStageBits::fragmentShader,
				vk::PipelineStageBits::allCommands,
				{}, {}, {}, {{barrier}});

			barrier.image = emissionImg;
			barrier.oldLayout = vk::ImageLayout::undefined;
			barrier.srcAccessMask = vk::AccessBits::shaderRead |
				vk::AccessBits::shaderWrite,
			barrier.newLayout = vk::ImageLayout::general;
			barrier.dstAccessMask = vk::AccessBits::shaderRead |
				vk::AccessBits::shaderWrite,
			barrier.subresourceRange.baseMipLevel = i + 1;
			vk::cmdPipelineBarrier(cb,
				vk::PipelineStageBits::allCommands,
				vk::PipelineStageBits::computeShader,
				{}, {}, {}, {{barrier}});

			horizontal = 0u;
			vk::cmdPushConstants(cb, bloom_.pipeLayout, vk::ShaderStageBits::compute,
				0, 4, &horizontal);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
				bloom_.pipeLayout, 0, {{emission.ds.vkHandle()}}, {});
			vk::cmdDispatch(cb, std::ceil(w / 32.f), std::ceil(h / 32.f), 1);
		}

		// make all meission layers readonly
		barrier.image = emission;
		barrier.oldLayout = vk::ImageLayout::general;
		barrier.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite,
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		barrier.subresourceRange.baseMipLevel = 1u;
		barrier.subresourceRange.levelCount = bloomLevels;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::allCommands,
			vk::PipelineStageBits::allCommands,
			{}, {}, {}, {{barrier}});
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 8u);

	// ssr pass
	if(params_.flags & flagSSR) {
		auto target = ssr_.target.vkImage();

		vk::ImageMemoryBarrier barrier;
		barrier.image = target;
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.srcAccessMask = {};
		barrier.newLayout = vk::ImageLayout::general;
		barrier.dstAccessMask = vk::AccessBits::shaderWrite;
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier}});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute,
			ssr_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			ssr_.pipeLayout, 0, {{sceneDs_.vkHandle(), ssr_.ds.vkHandle()}}, {});
		dlg_info("dispatch: {} {}", std::ceil(width / 8.f), std::ceil(height / 8.f));
		vk::cmdDispatch(cb, std::ceil(width / 8.f), std::ceil(height / 8.f), 1);

		barrier.oldLayout = vk::ImageLayout::general;
		barrier.srcAccessMask = vk::AccessBits::shaderWrite;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{barrier}});
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 9u);

	// combine pass
	if(params_.mode == 0) {
		auto& target = lpass_.light.image();

		vk::ImageMemoryBarrier barrier;
		barrier.image = target;
		barrier.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessBits::shaderRead;
		barrier.newLayout = vk::ImageLayout::general;
		barrier.dstAccessMask = vk::AccessBits::shaderWrite |
			 vk::AccessBits::shaderRead;
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::fragmentShader,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier}});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute,
			combine_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			combine_.pipeLayout, 0,
			{{sceneDs_.vkHandle(), combine_.ds.vkHandle()}}, {});
		vk::cmdDispatch(cb, std::ceil(width / 8.f), std::ceil(height / 8.f), 1u);

		barrier.oldLayout = vk::ImageLayout::general;
		barrier.srcAccessMask = vk::AccessBits::shaderRead |
			 vk::AccessBits::shaderRead;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{barrier}});
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 10u);

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

		if(params_.mode == 0) {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				pp_.pipeLayout, 0, {{pp_.ds.vkHandle()}}, {});
		} else {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, debug_.pipe);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				debug_.pipeLayout, 0, {{debug_.ds.vkHandle()}}, {});
		}

		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen

		// gui stuff
		rvgContext().bindDefaults(cb);
		gui().draw(cb);

		// times
		rvgContext().bindDefaults(cb);
		windowTransform().bind(cb);
		timings_.bgPaint.bind(cb);
		timings_.bg.fill(cb);

		timings_.fgPaint.bind(cb);
		for(auto& time : timings_.texts) {
			time.passName.draw(cb);
			time.time.draw(cb);
		}

		vk::cmdEndRenderPass(cb);
	}

	vk::cmdWriteTimestamp(cb, vk::PipelineStageBits::bottomOfPipe,
		queryPool_, 11u);

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

	auto update = camera_.update || updateLights_ || selectionPos_;
	update |= selectionPending_ || updateRenderParams_;
	if(update) {
		App::scheduleRedraw();
	}

	// TODO: only here for fps testing
	App::scheduleRedraw();
}

bool ViewApp::key(const ny::KeyEvent& ev) {
	static std::default_random_engine rnd(std::time(nullptr));
	if(App::key(ev)) {
		return true;
	}

	if(!ev.pressed) {
		return false;
	}

	auto numModes = 11u;
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
			params_.mode = (params_.mode + 1) % numModes;
			updateRenderParams_ = true;
			if(params_.mode == 0 || params_.mode == 1) {
				resize_ = true; // recreate, rerecord
			}
			return true;
		case ny::Keycode::l:
			params_.mode = (params_.mode + numModes - 1) % numModes;
			updateRenderParams_ = true;
			if(params_.mode == 0 || params_.mode == numModes - 1) {
				resize_ = true; // recreate, rerecord
			}
			return true;
		case ny::Keycode::equals:
			params_.exposure *= 1.1f;
			updateRenderParams_ = true;
			return true;
		case ny::Keycode::minus:
			params_.exposure /= 1.1f;
			updateRenderParams_ = true;
			return true;
		case ny::Keycode::comma: {
			auto& l = pointLights_.emplace_back(vulkanDevice(), lightDsLayout_,
				primitiveDsLayout_, shadowData_, currentID_++);
			std::uniform_real_distribution<float> dist(0.0f, 1.0f);
			l.data.position = camera_.pos;
			l.data.color = {dist(rnd), dist(rnd), dist(rnd)};
			l.data.attenuation = {1.f, 0.6f * dist(rnd), 0.3f * dist(rnd)};
			l.updateDevice();
			App::scheduleRerecord();
			break;
		 } case ny::Keycode::z: {
			 dlg_assert(dummyCube_.vkImageView());
			// no shadow map
			auto& l = pointLights_.emplace_back(vulkanDevice(), lightDsLayout_,
				primitiveDsLayout_, shadowData_, currentID_++,
				dummyCube_.vkImageView());
			std::uniform_real_distribution<float> dist(0.0f, 1.0f);
			l.data.position = camera_.pos;
			l.data.color = {dist(rnd), dist(rnd), dist(rnd)};
			l.data.attenuation = {1.f, 16, 256};
			l.data.radius = 0.5f;
			l.updateDevice();
			App::scheduleRerecord();
			break;
		} case ny::Keycode::f: {
			timings_.hidden ^= true;
			timings_.bg.disable(timings_.hidden);
			for(auto& t : timings_.texts) {
				t.passName.disable(timings_.hidden);
				t.time.disable(timings_.hidden);
			}
		} default:
			break;
	}

	auto uk = static_cast<unsigned>(ev.keycode);
	auto k1 = static_cast<unsigned>(ny::Keycode::k1);
	auto k0 = static_cast<unsigned>(ny::Keycode::k0);
	if(uk >= k1 && uk <= k0) {
		auto was0 = params_.mode == 0;
		auto diff = (1 + uk - k1) % numModes;
		params_.mode = diff;
		updateRenderParams_ = true;
		if(was0 || params_.mode == 0) {
			resize_ = true; // recreate, rerecord
		}
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
	} else if(ev.pressed && ev.button == ny::MouseButton::right) {
		auto ipos = ev.position;
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
	App::updateDevice();

	// update scene ubo
	if(camera_.update) {
		camera_.update = false;
		auto map = sceneUbo_.memoryMap();
		auto span = map.span();
		auto mat = matrix(camera_);
		doi::write(span, mat);
		doi::write(span, nytl::Mat4f(nytl::inverse(mat)));
		doi::write(span, camera_.pos);
		doi::write(span, camera_.perspective.near);
		doi::write(span, camera_.perspective.far);

		skybox_.updateDevice(fixedMatrix(camera_));

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

	if(updateRenderParams_) {
		updateRenderParams_ = false;
		auto map = paramsUbo_.memoryMap();
		auto span = map.span();
		doi::write(span, params_);
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
		// TODO
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
		device().queueSubmitter().add(si);

		// wait for blit to finish before continuing with rendering
		App::addSemaphore(selectionSemaphore_,
			vk::PipelineStageBits::colorAttachmentOutput);
		selectionPending_ = true;
		*/
	}

	if(updateDescriptors_) {
		updateDescriptors();
	}

	// query pool
	if(++timings_.frameCount == 30) {
		auto& dev = vulkanDevice();
		std::uint32_t queries[timestampCount + 1];

		// TODO: can cause deadlock...
		vk::getQueryPoolResults(dev, queryPool_, 0, timestampCount + 1,
			sizeof(queries), queries, 4, vk::QueryResultBits::withAvailability);
		timings_.frameCount = 0u;

		auto last = queries[0] & timestampBits_;
		for(auto i = 0u; i < timestampCount; ++i) {
			auto current = queries[i + 1] & timestampBits_;
			double diff = current - last;
			diff *= dev.properties().limits.timestampPeriod; // ns
			diff /= 1000 * 1000; // ms
			auto sdiff = std::to_string(diff);
			timings_.texts[i].time.change()->text = sdiff;
			last = current;
		}
	}
}

void ViewApp::resize(const ny::SizeEvent& ev) {
	App::resize(ev);
	selectionPos_ = {};
	camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
	camera_.update = true;

	auto pos = nytl::Vec2f(window().size());
	pos.x -= 180;
	pos.y = 30;

	for(auto i = 0u; i < timings_.texts.size(); ++i) {
		timings_.texts[i].passName.change()->position = pos;

		auto opos = pos;
		opos.x += 100;
		timings_.texts[i].time.change()->position = opos;

		pos.y += 20;
	}

	auto bgc = timings_.bg.change();
	bgc->position = nytl::Vec2f(window().size());
	bgc->position.x -= 200;
	bgc->position.y = 10;
	bgc->size.x = 180;
	bgc->size.y = pos.y + 10;
}

const vk::PipelineColorBlendAttachmentState& ViewApp::noBlendAttachment() {
	static constexpr vk::PipelineColorBlendAttachmentState state = {
		false, {}, {}, {}, {}, {}, {},
		vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a,
	};
	return state;
}

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
