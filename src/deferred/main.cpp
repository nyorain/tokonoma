#include "geomLight.hpp"
#include "bloom.hpp"
#include "ssr.hpp"
#include "ssao.hpp"
#include "ao.hpp"
#include "combine.hpp"
#include "luminance.hpp"
#include "postProcess.hpp"

#include <stage/app.hpp>
#include <stage/f16.hpp>
#include <stage/render.hpp>
#include <stage/camera.hpp>
#include <stage/app.hpp>
#include <stage/types.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>
#include <stage/defer.hpp>
#include <stage/scene/shape.hpp>
#include <stage/scene/light.hpp>
#include <stage/scene/scene.hpp>
#include <stage/scene/environment.hpp>
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
#include <shaders/deferred.pointScatter.frag.h>
#include <shaders/deferred.dirScatter.frag.h>
#include <shaders/deferred.debug.frag.h>

#include <cstdlib>
#include <random>

// list of missing features:
// - lens flare
// - better dof implementations, current impl is naive

// TODO: fix up query pool usage
// TODO: support transparency. Can be ordered for now, i.e. split
//   primitives into two groups in Scene: BLEND and non-blend material-based.
//   the blend ones are sorted in each updateDevice (either on gpu
//   or (probably better for the start) on a host visible buffer)
//   via draw indirect commands. Render it in geomLight pass if possible (no
//   bloom/ssr) otherwise we probably have to use extra pass.
//   When using extra pass, render skybox after that pass (depth optimization)
// TODO: group initial layout transitions from undefined layout to general
//   per frame? could do it externally, undefined -> targetScope()
//   for luminance (compute), ssr, ssao (compute)
// TODO: correctly use byRegion dependency flag where possible
// TODO: re-enable light scattering.
//   allow light scattering per light?
//   we could still apply if after lightning pass
//   probably best to just use one buffer for all lights then
//   probably not a bad idea to use that buffer with additional blending
//   as additional attachment in the light pass, since we render
//   point light box there already.
//   We probably can get away with even less samples if we use the better
//   4+1 blur (that will even be faster!) in combine
// TODO: support destroying pass render buffers like previously
//   done on deferred. Only create them is needed/pass is active.
// TODO: support dynamic shader reloading (from file) on pass (re-)creation?
// TODO: improve ssr performance. Try out linear sampling, at-least-one-stepsize,
//   maybe we can work out a variable step size algorithm?
// TODO: blur ssr in extra pass (with compute shader shared memory,
//   see ssrBlur.comp wip)? Might improve performance
//   and allow for greater blur. Also distribute factors (compute guass +
//   depth difference) via shared variables.
//   maybe just generate mipmaps for light buffer, i guess that is how
//   it's usually done.
// TODO: we should be able to always use the same attenuation (or at least
//   make it independent from radius) by normalizing distance from light
//   (dividing by radius) before caluclating attunation.
//   currently done, has some problems as well though, fix those.
//     -> how to handle light attenuation for point lights?
//      see scene.glsl:attenuation, does it make sense to give each light
//      its own parameters? normalize distance by radius before passing
//      it to attenuation? parameters that work fine for a normalized
//      distance are (1, 8, 128), but then lights are rather weak
// TODO: we could allow to display luminance mipmaps in debug shader
// TODO: parameterize all (currently somewhat random) values and factors
//   (especially in artefact-prone screen space shaders)
// TODO: rgba16snorm (normalsFormat) isn't guaranteed to be supported
//   as color attachment... i guess we could fall back to rgba16sint
//   which is guaranteed to be supported? and then encode it
// TODO: point light scattering currently doesn't support the ldv value,
//   i.e. view-dir dependent scattering only based on attenuation
// TODO: support light scattering for every light (add a light flag for it)?
//   For point lights, we only have to scatter in the light volume.
//   Probably can't combine it with the volume rasterization step in the
//   light pass though, so have to render the box twice.
//   not sure if really worth it for point lights though
// TODO: cascaded shadow maps for directional lights
//   Allow to set the shadow map size for all lights. Or automatically
//   dynamically allocate shadow maps per frame (but also see the
//   only one shadow map at all thing).
// TODO: do we really need depth mip levels anywhere? test if it really
//   brings ssao performance improvement. Otherwise we are wasting this
//   whole "depth mip map levels" thing
// TODO: reflectange probe rendering

// low prio:
// TODO: look at adding smaa. That needs multiple post processing
//   passes though... Add it as new class
// TODO: more an idea:
//   shadow cube debug mode (where cubemap is rendered, one can
//   look around). Maybe use moving the camera then for moving the light.
//   can be activated via vui when light is selcted.
//   something comparable for dir lights? where direction can be set by
//   moving camera around?

// lower prio optimizations (most of these are guesses):
// TODO(optimization): when ssr is disabled (and/or others?) we can probably do
//   combine and pp pass in one pass.
// TODO(optimization): the shadow pipelines currently bind and pass through
//   both tex coords, also have normals declared as input (which they don't
//   need). implement alternative primitive rendering mode, where it
//   just binds the texCoords buffer needed for albedo, and then use
//   a vertex input with just one texCoord and without normals in light
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
//   -> nothing culling-related implemented at all at the moment
// TODO: look into vk_khr_multiview (promoted to vulkan 1.1?) for
//   cubemap rendering (point lights, but also skybox transformation)
// TODO(optimization): we currently don't use the a component of the
//   bloom targets. We could basically get a free (strong) blur there...
//   maybe good for ssao or something?
// TODO: just treat light scattering as emission? get the strong blur for free.
//   might be way too strong though... not as alpha (see todo above) but simply
//   add it to the emission color

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

using namespace doi::types;
using doi::f16;

class ViewApp : public doi::App {
public:
	using Vertex = doi::Primitive::Vertex;

	static constexpr auto lightFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto normalsFormat = vk::Format::r16g16b16a16Snorm;
	static constexpr auto albedoFormat = vk::Format::r8g8b8a8Srgb;
	static constexpr auto emissionFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto scatterFormat = vk::Format::r8Unorm;
	static constexpr auto ldepthFormat = vk::Format::r16Sfloat;
	static constexpr auto pointLight = false;

	static constexpr u32 flagFXAA = (1u << 0u); // pp.frag

	static constexpr u32 passScattering = (1u << 1u);
	static constexpr u32 passSSR = (1u << 2u);
	static constexpr u32 passBloom = (1u << 3u);
	static constexpr u32 passSSAO = (1u << 4u);
	static constexpr u32 passLuminance = (1u << 5u);

	static constexpr u32 passAO = (1u << 6u);
	static constexpr u32 passCombine = (1u << 6u);

	static constexpr unsigned timestampCount = 13u;

public:
	bool init(const nytl::Span<const char*> args) override;
	void initRenderData() override;
	void initPasses(const doi::WorkBatcher&);
	// void initFwd();

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
		return {pp_.renderPass(), 0u};
	}

protected:
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

	doi::Texture brdfLut_;
	doi::Environment env_;

	u32 debugMode_ {0};
	u32 renderPasses_;

	// needed for point light rendering.
	// also needed for skybox
	vpp::SubBuffer boxIndices_;

	// image view into the depth buffer that accesses all depth levels
	vpp::ImageView depthMipView_;
	unsigned depthMipLevels_ {};

	// TODO: now that doi has a f16 class we could use a buffer as well.
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

	// vpp::QueryPool queryPool_ {};
	// std::uint32_t timestampBits_ {};

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

	GeomLightPass geomLight_;
	BloomPass bloom_;
	SSRPass ssr_;
	SSAOPass ssao_;
	AOPass ao_;
	CombinePass combine_;
	LuminancePass luminance_;
	PostProcessPass pp_;

	bool recreatePasses_ {false};
	vpp::ShaderModule fullVertShader_;

	TimeWidget timeWidget_;

	// // times
	// struct TimePass {
	// 	rvg::Text passName;
	// 	rvg::Text time;
	// };
//
	// struct {
	// 	std::array<TimePass, timestampCount> texts;
	// 	rvg::RectShape bg;
	// 	rvg::Paint bgPaint;
	// 	rvg::Paint fgPaint;
	// 	unsigned frameCount {0};
	// 	bool hidden {};
	// } timings_;

	struct {
		vui::dat::Label* luminance;
		vui::dat::Label* exposure;
		vui::dat::Label* focusDepth;
		vui::dat::Label* dofDepth;
	} gui_;
};

bool ViewApp::init(const nytl::Span<const char*> args) {
	if(!doi::App::init(args)) {
		return false;
	}

	camera_.perspective.near = 0.01f;
	camera_.perspective.far = 40.f;

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

	// auto createFlagCheckbox = [&](auto& at, auto name, auto& flags, auto flag,
	// 		bool recreate, auto* flags2 = {}, auto flag2 = 0u) {
	// 	auto& cb = at.template create<Checkbox>(name).checkbox();
	// 	cb.set(flags & flag);
	// 	cb.onToggle = [=, &flags](auto&) {
	// 		flags ^= flag;
	// 		ao_.params.flags ^= AOPass::flagSSAO;
	// 		if(recreate) {
	// 			this->App::resize_ = true; // recreate, rerecord
	// 		}
	// 		...
	// 	};
	// 	return cb;
	// };

	// == general/post processing folder ==
	auto& pp = panel.create<vui::dat::Folder>("Post Processing");
	// createValueTextfield(pp, "exposure", params_.exposure, flag);
	createValueTextfield(pp, "aoFactor", ao_.params.factor);
	createValueTextfield(pp, "ssaoPow", ao_.params.ssaoPow);
	createValueTextfield(pp, "tonemap", pp_.params.tonemap);
	createValueTextfield(pp, "scatter strength", combine_.params.scatterStrength);
	createValueTextfield(pp, "dof strength", combine_.params.scatterStrength);

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

	/*
	auto& cb8 = pp.create<Checkbox>("Scattering").checkbox();
	cb8.set(renderPasses_ & passScattering);
	cb8.onToggle = [&](auto&) {
		renderPasses_ ^= passScattering;
		combine_.params.flags ^= CombinePass::flagScattering;
		App::resize_ = true; // recreate, rerecord
	};
	*/

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
	cb5.set(pp_.params.flags & flagFXAA);
	cb5.onToggle = [&](auto&) {
		pp_.params.flags ^= flagFXAA;
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

	// == bloom folder ==
	auto& bf = panel.create<vui::dat::Folder>("Bloom");
	auto& cbbm = bf.create<Checkbox>("mip blurred").checkbox();
	cbbm.set(bloom_.mipBlurred);
	cbbm.onToggle = [&](auto&) {
		bloom_.mipBlurred ^= true;
		App::resize_ = true; // recreate, rerecord
	};
	auto& cbbd = bf.create<Checkbox>("decrease").checkbox();
	cbbd.set(combine_.params.flags & CombinePass::flagBloomDecrease);
	cbbd.onToggle = [&](auto&) {
		combine_.params.flags ^= CombinePass::flagBloomDecrease;
	};
	createValueTextfield(bf, "max levels", bloom_.maxLevels, &this->App::resize_);
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
	createValueTextfield(lf, "groupDimSize", luminance_.mipGroupDimSize, &recreatePasses_);

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

	timeWidget_ = {rvgContext(), defaultFont()};

	/*
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
			"ao:",
			"combine:",
			"luminance:",
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
	*/

	return true;
}

void ViewApp::initPasses(const doi::WorkBatcher& wb) {
	// bloom
	PassCreateInfo passInfo {
		wb,
		depthFormat(), {
			sceneDsLayout_,
			materialDsLayout_,
			primitiveDsLayout_,
			lightDsLayout_,
		}, {
			linearSampler_,
			nearestSampler_,
		},
		fullVertShader_
	};

	// NOTE: conservative simplification at the moment; can be optimzied
	renderPasses_ = doi::bit(renderPasses_, passAO,
		bool(renderPasses_ & passSSAO));
	renderPasses_ = doi::bit(renderPasses_, passCombine,
		bool(renderPasses_ & (passSSR | passBloom | passScattering)));

	vpp::InitObject initPP(pp_, passInfo, swapchainInfo().imageFormat);

	// geomLight dst scopes
	SyncScope dstLight = pp_.dstScopeInput();
	SyncScope dstDepth = {};
	SyncScope dstLDepth = {};
	SyncScope dstEmission = {};
	SyncScope dstAlbedo = {};
	SyncScope dstNormals = {};

	if(debugMode_ != 0) {
		dstDepth |= pp_.dstScopeInput();
		dstLDepth |= pp_.dstScopeInput();
		dstEmission |= pp_.dstScopeInput();
		dstAlbedo |= pp_.dstScopeInput();
		dstNormals |= pp_.dstScopeInput();
	} else if(pp_.params.flags & pp_.flagDOF) {
		dstDepth = pp_.dstScopeInput();
	}

	AOPass::InitData initAO;
	if(renderPasses_ & passAO) {
		ao_.create(initAO, passInfo);
		dstLDepth |= ao_.dstScopeGBuf();
		dstAlbedo |= ao_.dstScopeGBuf();
		dstNormals |= ao_.dstScopeGBuf();
		dstEmission |= ao_.dstScopeGBuf();
	}

	CombinePass::InitData initCombine;
	if(renderPasses_ & passCombine) {
		combine_.create(initCombine, passInfo);
		dstLDepth |= combine_.dstScopeDepth();
	}

	BloomPass::InitData initBloom;
	if(renderPasses_ & passBloom) {
		bloom_.create(initBloom, passInfo);
		// replace previous dependencies since bloom blits from
		// emission. Manually sync to following passes via barrier in record
		dstEmission = bloom_.dstScopeEmission();
	}

	SSRPass::InitData initSSR;
	if(renderPasses_ & passSSR) {
		ssr_.create(initSSR, passInfo);
		dstLDepth |= ssr_.dstScopeDepth();
		dstNormals |= ssr_.dstScopeNormals();
	}

	SSAOPass::InitData initSSAO;
	if(renderPasses_ & passSSAO) {
		ssao_.create(initSSAO, passInfo);
		dstLDepth |= ssao_.dstScopeDepth();
		dstNormals |= ssao_.dstScopeNormals();
	}

	LuminancePass::InitData initLuminance;
	if(renderPasses_ & passLuminance) {
		luminance_.create(initLuminance, passInfo);
	}

	vpp::InitObject initGeomLight(geomLight_, passInfo,
		dstNormals, dstAlbedo, dstEmission, dstDepth, dstLDepth, dstLight,
		!(renderPasses_ & passAO));

	// initFwd(); // init forward pass for skybox and transparent objects

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

	// others
	env_.createPipe(device(), geomLight_.renderPass(), 1,
		vk::SampleCountBits::e1);
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
	vpp::nameHandle(linearSampler_, "nearestSampler");

	fullVertShader_ = {dev, stage_fullscreen_vert_data};

	// general layouts
	auto stages =  vk::ShaderStageBits::vertex |
		vk::ShaderStageBits::fragment |
		vk::ShaderStageBits::compute;
	auto sceneBindings = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer, stages)
	};
	sceneDsLayout_ = {dev, sceneBindings};

	// TODO: use shadow sampler statically here?
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

	// per object; model matrix and material stuff
	auto objectBindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
	};
	primitiveDsLayout_ = {dev, objectBindings};
	materialDsLayout_ = doi::Material::createDsLayout(dev);

	// dummy texture for materials that don't have a texture
	// TODO: we could just create the dummy cube and make the dummy
	// texture just a view into one of the dummy cube faces...
	// TODO: those are required below (mainly by lights and by
	//   materials, both should be fixable).
	auto idata = std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u};
	auto span = nytl::as_bytes(nytl::span(idata));
	auto p = doi::wrap({1u, 1u}, vk::Format::r8g8b8a8Unorm, span);
	doi::TextureCreateParams params;
	params.format = vk::Format::r8g8b8a8Unorm;
	dummyTex_ = {batch, std::move(p), params};

	vpp::nameHandle(dummyTex_.image(), "dummyTex.image");
	vpp::nameHandle(dummyTex_.imageView(), "dummyTex.view");

	auto dptr = reinterpret_cast<const std::byte*>(idata.data());
	auto faces = {dptr, dptr, dptr, dptr, dptr, dptr};
	params.cubemap = true;
	p = doi::wrap({1u, 1u}, vk::Format::r8g8b8a8Unorm, 1, 1, 6u, faces);
	dummyCube_ = {batch, std::move(p), params};

	vpp::nameHandle(dummyCube_.image(), "dummyCube.image");
	vpp::nameHandle(dummyCube_.imageView(), "dummyCube.view");

	// ubo and stuff
	auto hostMem = dev.hostMemoryTypes();
	auto sceneUboSize = sizeof(nytl::Mat4f) // proj matrix
		+ sizeof(nytl::Mat4f) // inv proj
		+ sizeof(nytl::Vec3f) // viewPos
		+ 2 * sizeof(float); // near, far plane
	vpp::Init<vpp::TrDs> initSceneDs(batch.alloc.ds, sceneDsLayout_);
	vpp::Init<vpp::SubBuffer> initSceneUbo(batch.alloc.bufHost, sceneUboSize,
		vk::BufferUsageBits::uniformBuffer, hostMem);

	shadowData_ = doi::initShadowData(dev, depthFormat(),
		lightDsLayout_, materialDsLayout_, primitiveDsLayout_,
		doi::Material::pcr());

	vpp::Init<vpp::SubBuffer> initRenderStage(batch.alloc.bufHost,
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
	vpp::Init<vpp::Image> initSelectionStage(batch.alloc.memHost, imgi,
		dev.hostMemoryTypes());

	selectionCb_ = dev.commandAllocator().get(qfam,
		vk::CommandPoolCreateBits::resetCommandBuffer);
	selectionSemaphore_ = {dev};

	// TODO: we can't assume that timestamps are supported...
	// timestampBits_ = dev.queueSubmitter().queue().properties().timestampValidBits;
	// dlg_assert(timestampBits_);
	// timestampBits_ = 0xFFFFFFFFu; // guaranteed

	// environment
	doi::Environment::InitData envData;
	env_.create(envData, batch, "convolution.ktx",
		"irradiance.ktx", linearSampler_);
	vpp::Init<doi::Texture> initBrdfLut(batch, doi::read("brdflut.ktx"));
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

	initPasses(batch);

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
	env_.init(envData, batch);
	brdfLut_ = initBrdfLut.init(batch);
	renderStage_ = initRenderStage.init();

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

	// add lights
	// TODO: creation not deferred
	if(pointLight) {
		auto& l = pointLights_.emplace_back(batch, lightDsLayout_,
			primitiveDsLayout_, shadowData_, currentID_++);
		l.data.position = {-1.8f, 6.0f, -2.f};
		l.data.color = {2.f, 1.7f, 0.8f};
		l.data.color *= 2;
		l.data.attenuation = {1.f, 0.3f, 0.1f};
		// light radius must be smaller than far plane / 2
		l.data.radius = 9.f;
		l.updateDevice();
		// pp_.params.scatterLightColor = 0.1f * l.data.color;
	} else {
		auto& l = dirLights_.emplace_back(batch, lightDsLayout_,
			primitiveDsLayout_, shadowData_, camera_.pos, currentID_++);
		l.data.dir = {-3.8f, -9.2f, -5.2f};
		l.data.color = {2.f, 1.7f, 0.8f};
		l.data.color *= 2;
		l.updateDevice(camera_.pos);
		// pp_.params.scatterLightColor = 0.05f * l.data.color;
	}

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

}

/*
void ViewApp::initFwd() {
	// render pass
	auto& dev = device();
	std::array<vk::AttachmentDescription, 2u> attachments;

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
*/

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
	vpp::ViewableImage target = {device().devMemAllocator(), img, view,
		device().deviceMemoryTypes()};

	return target;
}

static constexpr auto defaultAttachmentUsage =
	vk::ImageUsageBits::colorAttachment |
	vk::ImageUsageBits::inputAttachment |
	vk::ImageUsageBits::transferDst |
	vk::ImageUsageBits::transferSrc |
	vk::ImageUsageBits::sampled;

void ViewApp::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	auto& dev = vulkanDevice();
	depthTarget() = createDepthTarget(size);

	std::vector<vk::ImageView> attachments;

	vpp::DeviceMemoryAllocator memStage(dev);
	vpp::BufferAllocator bufStage(memStage);
	auto& alloc = *this->alloc;

	// NOTE: no cb needed here
	// TODO: memStage,bufStage neither, theoretically...
	doi::WorkBatcher wb{dev, {}, {
			alloc.memDevice, alloc.memHost, memStage,
			alloc.bufDevice, alloc.bufHost, bufStage,
			dev.descriptorAllocator(),
		}
	};

	auto scatterView = dummyTex_.vkImageView();

	GeomLightPass::InitBufferData geomLightData;
	geomLight_.createBuffers(geomLightData, wb, size);

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

	// init
	geomLight_.initBuffers(geomLightData, size, depthTarget().vkImageView(),
		env_.irradiance().imageView(),
		env_.envMap().imageView(),
		env_.convolutionMipmaps(),
		brdfLut_.imageView());
	auto ppInput = geomLight_.lightTarget().vkImageView();

	auto bloomView = dummyTex_.vkImageView();
	if(renderPasses_ & passBloom) {
		bloom_.initBuffers(bloomData, geomLight_.lightTarget().imageView());
		bloomView = bloom_.fullView();
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

	pp_.updateInputs(ppInput,
		geomLight_.ldepthTarget().imageView(),
		geomLight_.normalsTarget().imageView(),
		geomLight_.albedoTarget().imageView(),
		ssaoView,
		ssrView,
		geomLight_.emissionTarget().imageView(),
		bloomView,
		lumView);

	// create swapchain framebuffers
	for(auto& buf : bufs) {
		buf.framebuffer = pp_.initFramebuffer(buf.imageView, size);
	}
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

	auto pos = nytl::Vec2f(window().size());
	pos.x -= 240;
	pos.y = 10;
	timeWidget_.start(cb, pos);

	// render shadow maps
	for(auto& light : pointLights_) {
		if(light.hasShadowMap()) {
			light.render(cb, shadowData_, scene_);
		}
	}
	for(auto& light : dirLights_) {
		light.render(cb, shadowData_, scene_);
	}

	timeWidget_.add("shadows");

	auto size = swapchainInfo().imageExtent;
	const auto width = size.width;
	const auto height = size.height;
	vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
	vk::cmdSetViewport(cb, 0, 1, vp);
	vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

	geomLight_.record(cb, size, sceneDs_, scene_, pointLights_,
		dirLights_, boxIndices_, &env_, timeWidget_);

	auto ldepthImage = geomLight_.ldepthTarget().vkImage();
	auto lightImage = geomLight_.lightTarget().vkImage();
	auto emissionImage = geomLight_.emissionTarget().vkImage();

	// == ssao pass ==
	if(renderPasses_ & passSSAO) {
		ssao_.record(cb, size, sceneDs_);
		timeWidget_.add("ssao");
	}

	// == bloom pass ==
	ImageBarrier lightBarrier;
	lightBarrier.image = lightImage;
	if(renderPasses_ & passBloom) {
		// make fwd pass visible
		// lightBarrier.dst = bloom_.dstScopeLight();
		// barrier(cb, {{lightBarrier}});

		bloom_.record(cb, emissionImage, {width, height});
		lightBarrier.src = lightBarrier.dst;
		timeWidget_.add("bloom");
	}

	// == ssr pass ==
	if(renderPasses_ & passSSR) {
		ssr_.record(cb, sceneDs_, {width, height});
		timeWidget_.add("ssr");
	}

	// == ao pass ==
	// make sure output from previous stages is readable in ao pass
	// lightBarrier.src is still correctly set; bloom or fwd pass
	ImageBarrier emissionBarrier;
	if(renderPasses_ & passAO) {
		lightBarrier.dst = ao_.scopeLight();

		if(renderPasses_ & passBloom) {
			emissionBarrier.image = emissionImage;
			emissionBarrier.src = bloom_.dstScopeEmission(); // read access
			emissionBarrier.dst = ao_.dstScopeGBuf();
		}

		ImageBarrier ssaoBarrier;
		if(renderPasses_ & passSSAO) {
			ssaoBarrier.src = ssao_.srcScopeTarget();
			ssaoBarrier.dst = ao_.dstScopeSSAO();
			ssaoBarrier.image = ssao_.target().image();
		}

		barrier(cb, {{lightBarrier, ssaoBarrier, emissionBarrier}});
		lightBarrier.src = lightBarrier.dst;

		ao_.record(cb, sceneDs_, size);
		timeWidget_.add("ao");
	}

	// == combine pass ==
	// make sure inputs for combine pass are readable
	ImageBarrier ppInputBarrier = lightBarrier;
	if(renderPasses_ & passCombine) {
		lightBarrier.dst = combine_.dstScopeLight();

		ImageBarrier ssrBarrier;
		ssrBarrier.src = ssr_.srcScopeTarget();
		ssrBarrier.dst = combine_.dstScopeSSR();
		ssrBarrier.image = ssr_.target().image();

		// add transitive dependency against potential
		// previous use
		emissionBarrier.src = emissionBarrier.dst; // read access
		emissionBarrier.dst = combine_.scopeTarget();

		ImageBarrier bloomBarrier;
		bloomBarrier.src = bloom_.srcScopeTarget();
		bloomBarrier.dst = combine_.dstScopeBloom();
		bloomBarrier.image = bloom_.target();
		bloomBarrier.subres.levelCount = bloom_.levelCount();

		barrier(cb, {{lightBarrier, ssrBarrier, bloomBarrier, emissionBarrier}});
		combine_.record(cb, size);
		timeWidget_.add("combine");

		lightBarrier.src = lightBarrier.dst;
		emissionBarrier.src = emissionBarrier.dst;

		ppInputBarrier = emissionBarrier;
		ppInputBarrier.image = geomLight_.emissionTarget().image();
	}

	// == luminance pass ==
	if(renderPasses_ & (passCombine | passAO)) {
		ppInputBarrier.dst = luminance_.dstScopeLight() | pp_.dstScopeInput();
		barrier(cb, {{ppInputBarrier}});
	}

	if(renderPasses_ & passLuminance) {
		luminance_.record(cb, size);
		timeWidget_.add("luminance");

		if(debugMode_ != 0) {
			// make sure luminance is finished
			ImageBarrier lumBarrier;
			lumBarrier.image = luminance_.target().image();
			lumBarrier.src = luminance_.srcScopeTarget();
			lumBarrier.dst = pp_.dstScopeInput();
			barrier(cb, {{lumBarrier}});
		}
	}

	// == post process/debug output ===
	// make sure everything is readable in post-process/debug pass

	{
		vk::cmdBeginRenderPass(cb, {
			pp_.renderPass(),
			buf.framebuffer,
			{0u, 0u, width, height},
			0, nullptr
		}, {});

		pp_.record(cb, debugMode_);
		timeWidget_.add("pp");

		// gui stuff
		vpp::DebugLabel debugLabel(cb, "GUI");
		rvgContext().bindDefaults(cb);
		gui().draw(cb);

		// times
		timeWidget_.finish();
		rvgContext().bindDefaults(cb);
		windowTransform().bind(cb);
		timeWidget_.draw(cb);

		/*
		timings_.bgPaint.bind(cb);
		timings_.bg.fill(cb);

		timings_.fgPaint.bind(cb);
		for(auto& time : timings_.texts) {
			time.passName.draw(cb);
			time.time.draw(cb);
		}
		*/

		vk::cmdEndRenderPass(cb);
	}

	if(pp_.params.flags & pp_.flagDOF) {
		barrier(cb, ldepthImage, pp_.dstScopeInput(), {
			vk::PipelineStageBits::transfer,
			vk::ImageLayout::transferSrcOptimal,
			vk::AccessBits::transferRead,
		});

		vk::BufferImageCopy copy;
		copy.bufferOffset = renderStage_.offset();
		copy.imageExtent = {1, 1, 1};
		copy.imageOffset = {i32(width / 2u), i32(height / 2u), 0};
		copy.imageSubresource.aspectMask = vk::ImageAspectBits::color;
		copy.imageSubresource.layerCount = 1;
		copy.imageSubresource.mipLevel = 0;
		vk::cmdCopyImageToBuffer(cb, ldepthImage,
			vk::ImageLayout::transferSrcOptimal,
			renderStage_.buffer(), {{copy}});
	}

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

	// TODO: something about potential changes in pass parameters
	auto update = camera_.update || updateLights_ || selectionPos_;
	update |= selectionPending_;
	if(update) {
		App::scheduleRedraw();
	}

	// TODO: only here for fps testing, could be removed to not
	// render when not needed
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

	auto numModes = 12u;
	switch(ev.keycode) {
		case ny::Keycode::m: // move light here
			if(!dirLights_.empty()) {
				dirLights_[0].data.dir = -nytl::normalized(camera_.pos);
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
			debugMode_ = (debugMode_ + 1) % numModes;
			recreatePasses_ = true; // recreate, rerecord
			return true;
		case ny::Keycode::l:
			debugMode_ = (debugMode_ + numModes - 1) % numModes;
			recreatePasses_ = true; // recreate, rerecord
			return true;
		case ny::Keycode::equals:
			desiredLuminance_ *= 1.1f;
			return true;
		case ny::Keycode::minus:
			desiredLuminance_ /= 1.1f;
			return true;
		// TODO: re-add with work batcher
		/*
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
		} */case ny::Keycode::f: {
			// timings_.hide ^= true;
			// timings_.bg.disable(timings_.hidden);
			// for(auto& t : timings_.texts) {
			// 	t.passName.disable(timings_.hidden);
			// 	t.time.disable(timings_.hidden);
			// }
		} default:
			break;
	}

	auto uk = static_cast<unsigned>(ev.keycode);
	auto k1 = static_cast<unsigned>(ny::Keycode::k1);
	auto k0 = static_cast<unsigned>(ny::Keycode::k0);
	if(uk >= k1 && uk < k0) {
		auto diff = (1 + uk - k1) % numModes;
		debugMode_ = diff;
		recreatePasses_ = true; // recreate, rerecord
	} else if(ev.keycode == ny::Keycode::k0) {
		debugMode_ = 0;
		recreatePasses_ = true; // recreate, rerecord
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
	if(recreatePasses_) {
		recreatePasses_ = false;
		auto wb = doi::WorkBatcher::createDefault(device());
		auto& qs = device().queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = device().commandAllocator().get(qfam);
		wb.cb = cb;

		vk::beginCommandBuffer(cb, {});
		initPasses(wb);

		// initPasses reloads all pipelines, shouldn't happen in
		// normal rendering. So waiting here shouldn't be problem
		vk::endCommandBuffer(cb);
		auto id = qs.add(cb);
		qs.wait(id);

		App::resize_ = true; // recreate buffers and stuff
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
	pp_.updateDevice();

	// TODO: needed from update stage. Probably better to do all calculations
	// just there though probably
	float dt = 1 / 60.f;

	// update dof focus
	if(pp_.params.flags & pp_.flagDOF) {
		auto map = renderStage_.memoryMap();
		map.invalidate();
		auto span = map.span();

		auto focus = float(doi::read<f16>(span));
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
		pp_.params.exposure += 50 * dt * diff;
		gui_.exposure->label(std::to_string(pp_.params.exposure));
	} else {
		pp_.params.exposure = desiredLuminance_ / 0.2; // rather random
		gui_.luminance->label("-");
		gui_.exposure->label(std::to_string(pp_.params.exposure));
	}

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

		env_.updateDevice(fixedMatrix(camera_));

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
		device().queueSubmitter().add(si);

		// wait for blit to finish before continuing with rendering
		App::addSemaphore(selectionSemaphore_,
			vk::PipelineStageBits::colorAttachmentOutput);
		selectionPending_ = true;
		*/
	}

	/*
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
	*/

	timeWidget_.updateDevice();

	// NOTE: not sure where to put that. after rvg label changes here
	// because it updates rvg
	App::updateDevice();
}

void ViewApp::resize(const ny::SizeEvent& ev) {
	App::resize(ev);
	selectionPos_ = {};
	camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
	camera_.update = true;

	/*
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
	*/
}

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
