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

#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <vui/textfield.hpp>
#include <vui/colorPicker.hpp>

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
#include <shaders/deferred.pointLight.frag.h>
#include <shaders/deferred.dirLight.frag.h>
#include <shaders/deferred.ssao.frag.h>
#include <shaders/deferred.pointScatter.frag.h>
#include <shaders/deferred.dirScatter.frag.h>
#include <shaders/deferred.gblur.comp.h>
#include <shaders/deferred.ssr.comp.h>

#include <cstdlib>
#include <random>

// TODO: try out fxaa in postprocessing shader. Should it be done before
//   any effects like adding bloom, ssr, light scattering or ssao?
//   maybe try full image first, should work alright
// TODO: there is currently a artefact for light scattering.
//   was introduct with using the linear depth buffer
//   fix that (can e.g. be seen on sponza with point light).
//   ok, so not really an artefact and rather a structural problem:
//   the ground is in front of the light source from the
//   camera pov... i don't think there is a solution for that!
//   depth-based light scattering will probably only work for direcitonal
//   lights. The shadowmap-based approach should work for point lights
//   as well though (should work even better in general!)
//   integrate that.
//   nvm, seems to be something about ldv?!
// TODO: fix used samplers, we need more than one! some passes are
//   better with linear sampler, others need nearest sampler.
//   same for mipmap. But try to let multiple passes use same
//   sampler if possible
// TODO: use bright reflective areas (or just fully lit areas,
//   everything over a certain bloom threshold) for bloom as well,
//   not just the emissisve stuff. Simply output them into the emissive
//   buffer as well; we have no use for that besides blur.
//   But then scale emissive in there (like 0.9 * emissive + 0.1 * light)
//   http://kalogirou.net/2006/05/20/how-to-do-good-bloom-for-hdr-rendering/
// TODO: try out compute pipeline for ssao. vulkan does not require
//   that r8 formats support storage image though
// TODO: try out other mechanisms for better ssao cache coherency
//   we should somehow use a higher lod level i guess
// TODO: fix theoretical synchronization issues
// TODO: lod depth levels with nearest filter (see image blit)
//   aren't really helpful, linear depth mipmaps would be *way* better.
//   we could do that in a custom shader, should probably not even be
//   much more expensive
//   Probably quite nice alternative: use custom depth gbuffer that
//   stores linear depth (or even world position?) in r16f format.
//   should be enough for our algorithms
// TODO: investigate reversed z buffer
//   http://dev.theomader.com/depth-precision/
//   https://nlguillemot.wordpress.com/2016/12/07/reversed-z-in-opengl/

// NOTE: we always use ssao, even when object/material has ao texture.
// In that case both are multiplied. That's how its usually done, see
// https://docs.unrealengine.com/en-us/Engine/Rendering/LightingAndShadows/AmbientOcclusion

// NOTE: ssao currently independent from *any* lights at all
// NOTE: we currently completely ignore tangents and compute that
//   in the shader via derivates

// low prio optimizations:
// TODO(optimization?): we could theoretically just use one shadow map at all.
//   requires splitting the light passes though (rendering with light pipe)
//   so might have disadvantage. so not high prio
// TODO(optimization?) we could reuse float gbuffers for later hdr rendering
//   not that easy though since normal buffer has snorm format...
//   attachments can be used as color *and* input attachments in a
//   subpass. And reusing the normals buffer as light output buffer
//   is pretty perfect, no pass depends on both as input.
//   maybe vulkan mutable format helps?
// TODO(optimization): more efficient point light shadow cube map
//   rendering: only render those sides that we can actually see...
//   -> nothing culling-related implement at all at the moment
// TODO(optimization): we currently don't use the a component of the
//   bloom targets. We could basically get a free (strong) blur there...
//   maybe good for ssao or something?
// NOTE: investigate/compare deferred lightning elements?
//   http://gameangst.com/?p=141


class ViewApp : public doi::App {
public:
	using Vertex = doi::Primitive::Vertex;

	static constexpr auto lightFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto normalsFormat = vk::Format::r16g16b16a16Snorm;
	static constexpr auto albedoFormat = vk::Format::r8g8b8a8Srgb;
	static constexpr auto emissionFormat = vk::Format::r8g8b8a8Unorm;
	static constexpr auto ssaoFormat = vk::Format::r8Unorm;
	static constexpr auto scatterFormat = vk::Format::r8Unorm;
	static constexpr auto ldepthFormat = vk::Format::r16Sfloat;
	static constexpr auto ssrFormat = vk::Format::r8g8b8a8Unorm;
	static constexpr auto ssaoSampleCount = 16u;
	static constexpr auto pointLight = true;
	static constexpr auto maxBloomLevels = 4u;

	// pp.frag
	static constexpr unsigned flagSSAO = (1u << 0u);
	static constexpr unsigned flagScattering = (1u << 1u);
	static constexpr unsigned flagSSR = (1u << 2u);
	static constexpr unsigned flagBloom = (1u << 3u);

	// see pp.frag
	struct PostProcessParams { // could name that PPP
		nytl::Vec3f scatterLightColor {1.f, 0.9f, 0.5f};
		std::uint32_t tonemap {2}; // uncharted
		float aoFactor {0.05f};
		float ssaoPow {3.f};
		float exposure {1.f};
		std::uint32_t flags {flagBloom};
		std::uint32_t bloomLevels {1};
	};

	static const vk::PipelineColorBlendAttachmentState& noBlendAttachment();

public:
	bool init(const nytl::Span<const char*> args) override;
	void initRenderData() override;

	void initGPass();
	void initLPass();
	void initPPass();
	void initSSAO();
	void initScattering();
	void initBloom();
	void initSSR();
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

	vpp::Sampler sampler_; // default linear sampler
	vpp::ViewableImage dummyTex_;
	std::unique_ptr<doi::Scene> scene_;
	std::optional<doi::Material> lightMaterial_;

	std::string modelname_ {};
	float sceneScale_ {1.f};
	float maxAnisotropy_ {16.f};

	bool anisotropy_ {}; // whether device supports anisotropy
	std::uint32_t showMode_ {}; // what to show/debug views
	float time_ {};
	bool rotateView_ {}; // mouseLeft down
	doi::Camera camera_ {};
	bool updateLights_ {true};
	bool updateDescriptors_ {false};

	// doi::Skybox skybox_;

	// image view into the depth buffer that accesses all depth levels
	vpp::ImageView depthMipView_;
	unsigned depthMipLevels_ {};

	// 1x1px host visible image used to copy the selected model id into
	vpp::Image selectionStage_;
	std::optional<nytl::Vec2ui> selectionPos_;
	vpp::CommandBuffer selectionCb_;
	vpp::Semaphore selectionSemaphore_;
	bool selectionPending_ {};

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
		vpp::ViewableImage light;
		vpp::TrDsLayout dsLayout; // input
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pointPipe;
		vpp::Pipeline dirPipe;
	} lpass_;

	// post processing
	struct {
		vpp::RenderPass pass;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::SubBuffer ubo;
		vpp::Pipeline pipe;

		PostProcessParams params;
		bool updateParams {true};
	} pp_;

	// ssao
	struct {
		vpp::RenderPass pass;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::ViewableImage target;
		vpp::ViewableImage noise;
		vpp::Framebuffer fb;
		vpp::SubBuffer samples;
	} ssao_;

	// light scattering
	struct {
		vpp::RenderPass pass;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pointPipe;
		vpp::Pipeline dirPipe;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;

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
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDsLayout dsLayout;
		vpp::Image tmpTarget;
		std::vector<BloomLevel> tmpLevels;
		std::vector<BloomLevel> emissionLevels;
		vpp::ImageView fullBloom;
	} bloom_;

	// ssr
	struct {
		vpp::ViewableImage target;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
	} ssr_;
};

bool ViewApp::init(const nytl::Span<const char*> args) {
	if(!doi::App::init(args)) {
		return false;
	}

	auto& dev = vulkanDevice();
	camera_.perspective.near = 0.1f;
	camera_.perspective.far = 100.f;

	// TODO: re-add this. In pass between light and pp?
	// hdr skybox
	// skybox_.init(dev, "../assets/kloofendal2k.hdr",
		// renderPass(), 3u, samples());

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
	if(!(scene_ = doi::loadGltf(modelname_, dev, mat, ri))) {
		return false;
	}

	lightMaterial_.emplace(materialDsLayout_,
		dummyTex_.vkImageView(), scene_->defaultSampler(),
		nytl::Vec{1.f, 1.f, 0.4f, 1.f});

	if(pointLight) {
		auto& l = pointLights_.emplace_back(dev, lightDsLayout_,
			primitiveDsLayout_, shadowData_, *lightMaterial_);
		l.data.position = {-1.8f, 6.0f, -2.f};
		l.data.color = {2.f, 1.7f, 0.8f};
		l.data.attenuation = {1.f, 0.4f, 0.2f};
		l.updateDevice();
		pp_.params.scatterLightColor = 0.1f * l.data.color;
	} else {
		auto& l = dirLights_.emplace_back(dev, lightDsLayout_,
			primitiveDsLayout_, shadowData_, camera_.pos, *lightMaterial_);
		l.data.dir = {-3.8f, -9.2f, -5.2f};
		l.data.color = {2.f, 1.7f, 0.8f};
		l.updateDevice(camera_.pos);
		pp_.params.scatterLightColor = 0.5f * l.data.color;
	}

	// gui
	auto& gui = this->gui();

	using namespace vui::dat;
	auto pos = nytl::Vec2f {100.f, 0};
	auto& panel = gui.create<vui::dat::Panel>(pos, 300.f);

	auto& pp = panel.create<vui::dat::Folder>("Post Processing");
	auto createValueTextfield = [](auto& at, auto name, auto& value,
			bool* set = {}) {
		auto start = std::to_string(value);
		if(start.size() > 4) {
			start.resize(4, '\0');
		}
		auto& t = at.template create<Textfield>(name, start).textfield();
		t.onSubmit = [&, set, name](auto& tf) {
			try {
				value = std::stof(tf.utf8());
				if(set) {
					*set = true;
				}
			} catch(const std::exception& err) {
				dlg_error("Invalid float for {}: {}", name, tf.utf8());
				return;
			}
		};
	};

	auto set = &pp_.updateParams;
	createValueTextfield(pp, "exposure", pp_.params.exposure, set);
	createValueTextfield(pp, "aoFactor", pp_.params.aoFactor, set);
	createValueTextfield(pp, "ssaoPow", pp_.params.ssaoPow, set);
	createValueTextfield(pp, "tonemap", pp_.params.tonemap, set);

	auto& cb1 = pp.create<Checkbox>("ssao").checkbox();
	cb1.set(pp_.params.flags & flagSSAO);
	cb1.onToggle = [&](auto&) {
		pp_.params.flags ^= flagSSAO;
		pp_.updateParams = true;
		updateDescriptors_ = true;
		App::scheduleRerecord();
	};

	auto& cb2 = pp.create<Checkbox>("scattering").checkbox();
	cb2.set(pp_.params.flags & flagScattering);
	cb2.onToggle = [&](auto&) {
		pp_.params.flags ^= flagScattering;
		pp_.updateParams = true;
		updateDescriptors_ = true;
		App::scheduleRerecord();
	};

	auto& cb3 = pp.create<Checkbox>("SSR").checkbox();
	cb3.set(pp_.params.flags & flagSSR);
	cb3.onToggle = [&](auto&) {
		pp_.params.flags ^= flagSSR;
		pp_.updateParams = true;
		updateDescriptors_ = true;
		App::scheduleRerecord();
	};

	auto& cb4 = pp.create<Checkbox>("Bloom").checkbox();
	cb4.set(pp_.params.flags & flagBloom);
	cb4.onToggle = [&](auto&) {
		pp_.params.flags ^= flagBloom;
		pp_.updateParams = true;
		updateDescriptors_ = true;
		App::scheduleRerecord();
	};

	return true;
}

void ViewApp::initRenderData() {
	auto& dev = vulkanDevice();

	// sampler
	vk::SamplerCreateInfo sci;
	sci.addressModeU = vk::SamplerAddressMode::clampToEdge;
	sci.addressModeV = vk::SamplerAddressMode::clampToEdge;
	sci.addressModeW = vk::SamplerAddressMode::clampToEdge;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	// sci.magFilter = vk::Filter::nearest;
	// sci.minFilter = vk::Filter::nearest;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	// sci.mipmapMode = vk::SamplerMipmapMode::linear;
	sci.minLod = 0.0;
	sci.maxLod = 100.0;
	// sci.maxLod = 0.0;
	sci.anisotropyEnable = false;
	// sci.anisotropyEnable = anisotropy_;
	// sci.maxAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
	sampler_ = {dev, sci};

	initGPass(); // geometry to g buffers
	initLPass(); // per light: using g buffers for shading
	initPPass(); // post processing, combining
	initSSAO(); // ssao
	initScattering(); // light scattering/volumentric light shafts/god rays
	initBloom(); // blur light regions
	initSSR(); // screen space relfections

	// dummy texture for materials that don't have a texture
	std::array<std::uint8_t, 4> data{255u, 255u, 255u, 255u};
	auto ptr = reinterpret_cast<std::byte*>(data.data());
	dummyTex_ = doi::loadTexture(dev, {1, 1, 1},
		vk::Format::r8g8b8a8Unorm, {ptr, data.size()});

	// ubo and stuff
	auto sceneUboSize = sizeof(nytl::Mat4f) // proj matrix
		+ sizeof(nytl::Mat4f) // inv proj
		+ sizeof(nytl::Vec3f) // viewPos
		+ 2 * sizeof(float) // near, far plane
		+ sizeof(std::uint32_t); // render/show buffer mode; flags
	sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
	sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
		vk::BufferUsageBits::uniformBuffer, 0, dev.hostMemoryTypes()};

	shadowData_ = doi::initShadowData(dev, depthFormat(),
		lightDsLayout_, materialDsLayout_, primitiveDsLayout_,
		doi::Material::pcr());

	// scene descriptor, used for some pipelines as set 0 for camera
	// matrix and view position
	vpp::DescriptorSetUpdate sdsu(sceneDs_);
	sdsu.uniform({{sceneUbo_.buffer(), sceneUbo_.offset(), sceneUbo_.size()}});
	vpp::apply({sdsu});

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
	selectionStage_ = {dev, imgi, dev.hostMemoryTypes()};

	// TODO: batch...
	vpp::changeLayout(selectionStage_,
		vk::ImageLayout::undefined,
		vk::PipelineStageBits::topOfPipe, {},
		vk::ImageLayout::general,
		vk::PipelineStageBits::bottomOfPipe, {},
		{vk::ImageAspectBits::color, 0, 1, 0, 1}, dev.queueSubmitter());

	auto qfam = dev.queueSubmitter().queue().family();
	selectionCb_ = dev.commandAllocator().get(qfam,
		vk::CommandPoolCreateBits::resetCommandBuffer);
	selectionSemaphore_ = {dev};
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

	gpass_.pipeLayout = {dev, {
		sceneDsLayout_,
		materialDsLayout_,
		primitiveDsLayout_,
	}, {pcr}};

	vpp::ShaderModule vertShader(dev, deferred_gbuf_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_gbuf_frag_data);
	vpp::GraphicsPipelineInfo gpi {gpass_.pass, gpass_.pipeLayout, {{
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
	gbufRefs[2].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	gbufRefs[3].attachment = ids.depth;
	gbufRefs[3].layout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::AttachmentReference lightRef;
	lightRef.attachment = ids.light;
	lightRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &lightRef;
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
		vk::AccessBits::shaderRead;

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

	lpass_.pipeLayout = {dev, {sceneDsLayout_, lpass_.dsLayout,
		lightDsLayout_}, {}};

	// TODO: don't use fullscreen here. Only render the areas of the screen
	// that are effected by the light (huge, important optimization
	// when there are many lights in the scene!)
	// Render simple box volume for best performance
	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule pointFragShader(dev, deferred_pointLight_frag_data);
	vpp::GraphicsPipelineInfo pgpi{lpass_.pass, lpass_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{pointFragShader, vk::ShaderStageBits::fragment},
	}}};

	pgpi.depthStencil.depthTestEnable = false;
	pgpi.depthStencil.depthWriteEnable = false;
	pgpi.assembly.topology = vk::PrimitiveTopology::triangleFan;

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

	pgpi.blend.attachmentCount = 1u;
	pgpi.blend.pAttachments = &blendAttachment;
	pgpi.flags(vk::PipelineCreateBits::allowDerivatives);

	// dir light
	// here we can use a fullscreen shader pass since directional lights
	// don't have a light volume
	vpp::ShaderModule dirFragShader(dev, deferred_dirLight_frag_data);
	vpp::GraphicsPipelineInfo lgpi{lpass_.pass, lpass_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{dirFragShader, vk::ShaderStageBits::fragment},
	}}};

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
	// std::array<vk::AttachmentDescription, 3u> attachments;
	std::array<vk::AttachmentDescription, 2u> attachments;
	struct {
		unsigned swapchain = 0u;
		// unsigned light = 1u;
		// unsigned albedo = 2u;
		unsigned albedo = 1u;
	} ids;

	attachments[ids.swapchain].format = swapchainInfo().imageFormat;
	attachments[ids.swapchain].samples = samples();
	attachments[ids.swapchain].loadOp = vk::AttachmentLoadOp::dontCare;
	attachments[ids.swapchain].storeOp = vk::AttachmentStoreOp::store;
	attachments[ids.swapchain].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[ids.swapchain].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[ids.swapchain].initialLayout = vk::ImageLayout::undefined;
	attachments[ids.swapchain].finalLayout = vk::ImageLayout::presentSrcKHR;

	auto addInput = [&](auto id, auto format) {
		attachments[id].format = format;
		attachments[id].samples = samples();
		attachments[id].loadOp = vk::AttachmentLoadOp::load;
		attachments[id].storeOp = vk::AttachmentStoreOp::store;
		attachments[id].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[id].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[id].initialLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[id].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	};

	// addInput(ids.light, lightFormat);
	addInput(ids.albedo, albedoFormat);

	// subpass
	vk::AttachmentReference inputRefs[2];
	// inputRefs[0].attachment = ids.light;
	// inputRefs[0].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	// inputRefs[1].attachment = ids.albedo;
	// inputRefs[1].layout = vk::ImageLayout::shaderReadOnlyOptimal;
	inputRefs[0].attachment = ids.albedo;
	inputRefs[0].layout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::AttachmentReference colorRef;
	colorRef.attachment = ids.swapchain;
	colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;
	// subpass.inputAttachmentCount = 2u;
	subpass.inputAttachmentCount = 1u;
	subpass.pInputAttachments = inputRefs;

	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	pp_.pass = {dev, rpi};

	// pipe
	auto lightInputBindings = {
		// vpp::descriptorBinding( // output from light pass
		// 	vk::DescriptorType::inputAttachment,
		// 	vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // output from light pass
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // albedo for ao
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // params ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // ssao
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // light scattering
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // normal
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // bloom
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // ssr
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
	};

	pp_.dsLayout = {dev, lightInputBindings};
	pp_.ds = {device().descriptorAllocator(), pp_.dsLayout};

	auto uboSize = sizeof(PostProcessParams);
	pp_.ubo = {dev.bufferAllocator(), uboSize,
		vk::BufferUsageBits::uniformBuffer, 0, dev.hostMemoryTypes()};

	pp_.pipeLayout = {dev, {sceneDsLayout_, pp_.dsLayout}, {}};

	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule fragShader(dev, deferred_pp_frag_data);
	vpp::GraphicsPipelineInfo gpi {pp_.pass, pp_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment},
	}}};

	gpi.depthStencil.depthTestEnable = false;
	gpi.depthStencil.depthWriteEnable = false;
	gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	gpi.blend.attachmentCount = 1u;
	gpi.blend.pAttachments = &noBlendAttachment();

	vk::Pipeline vkpipe;
	vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), nullptr, vkpipe);

	pp_.pipe = {dev, vkpipe};
}

void ViewApp::initSSAO() {
	// render pass
	auto& dev = device();
	std::array<vk::AttachmentDescription, 2u> attachments;

	// ssao image output format
	attachments[0].format = ssaoFormat;
	attachments[0].samples = vk::SampleCountBits::e1;
	attachments[0].loadOp = vk::AttachmentLoadOp::dontCare;
	attachments[0].storeOp = vk::AttachmentStoreOp::store;
	attachments[0].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[0].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[0].initialLayout = vk::ImageLayout::undefined;
	attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	// normals input attachment
	attachments[1].format = normalsFormat;
	attachments[1].samples = samples();
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

	vk::AttachmentReference inputRef;
	inputRef.attachment = 1u;
	inputRef.layout = vk::ImageLayout::shaderReadOnlyOptimal;

	vk::SubpassDescription subpass;
	subpass.colorAttachmentCount = 1u;
	subpass.pColorAttachments = &colorRef;
	subpass.inputAttachmentCount = 1u;
	subpass.pInputAttachments = &inputRef;

	// TODO: we currently have no barrier after ssao pass and before
	// post process pass... probably best done via pipeline barrier
	// or as input dependency in post processing pass.
	// output dependency here would be bad since we might have other
	// (independent) passes after ssao and before post process that
	// can run in parallel
	vk::RenderPassCreateInfo rpi;
	rpi.attachmentCount = attachments.size();
	rpi.pAttachments = attachments.data();
	rpi.subpassCount = 1u;
	rpi.pSubpasses = &subpass;

	ssao_.pass = {dev, rpi};

	// pipeline
	auto ssaoBindings = {
		vpp::descriptorBinding( // ssao samples and such
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::fragment),
		vpp::descriptorBinding( // ssao noise texture
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // depth texture
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // normals input
			vk::DescriptorType::inputAttachment,
			vk::ShaderStageBits::fragment),
	};

	ssao_.dsLayout = {dev, ssaoBindings};
	ssao_.pipeLayout = {dev, {sceneDsLayout_, ssao_.dsLayout}, {}};

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
	vpp::GraphicsPipelineInfo gpi {ssao_.pass, ssao_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{fragShader, vk::ShaderStageBits::fragment, &spi},
	}}};

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
	ssao_.samples = {dev.bufferAllocator(), size, usage, 0, devMem};
	vpp::writeStaging140(ssao_.samples, vpp::raw(*ssaoKernel.data(),
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
	ssao_.noise = doi::loadTexture(dev, {ssaoNoiseDim, ssaoNoiseDim, 1u},
		vk::Format::r32g32b32a32Sfloat, {ptr, ptrSize});

	ssao_.ds = {dev.descriptorAllocator(), ssao_.dsLayout};
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
			vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
	};

	scatter_.dsLayout = {dev, scatterBindings};
	scatter_.pipeLayout = {dev, {sceneDsLayout_, scatter_.dsLayout,
		lightDsLayout_}, {}};

	// TODO: at least for point light, we don't have to use a fullscreen
	// pass here! that really should bring quite the improvement (esp
	// if we later on allow multiple point light scattering effects)
	// TODO: fullscreen shader used by multiple passes, don't reload
	// it every time...
	vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
	vpp::ShaderModule pfragShader(dev, deferred_pointScatter_frag_data);
	vpp::GraphicsPipelineInfo pgpi{scatter_.pass, scatter_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{pfragShader, vk::ShaderStageBits::fragment},
	}}};

	pgpi.flags(vk::PipelineCreateBits::allowDerivatives);
	pgpi.depthStencil.depthTestEnable = false;
	pgpi.depthStencil.depthWriteEnable = false;
	pgpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
	pgpi.blend.attachmentCount = 1u;
	pgpi.blend.pAttachments = &noBlendAttachment();

	// directionoal
	vpp::ShaderModule dfragShader(dev, deferred_dirScatter_frag_data);
	vpp::GraphicsPipelineInfo dgpi{scatter_.pass, scatter_.pipeLayout, {{
		{vertShader, vk::ShaderStageBits::vertex},
		{dfragShader, vk::ShaderStageBits::fragment},
	}}};

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
		vpp::descriptorBinding( // input color
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // output color, blurred
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute)
	};

	vk::PushConstantRange pcr;
	pcr.size = 4;
	pcr.stageFlags = vk::ShaderStageBits::compute;

	bloom_.dsLayout = {dev, blurBindings};
	bloom_.pipeLayout = {dev, {bloom_.dsLayout}, {pcr}};

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
			vk::ShaderStageBits::compute, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // normals
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding( // output data
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute)
	};

	ssr_.dsLayout = {dev, ssrBindings};
	ssr_.ds = {dev.descriptorAllocator(), ssr_.dsLayout};
	ssr_.pipeLayout = {dev, {sceneDsLayout_, ssr_.dsLayout}, {}};

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
	vpp::ViewableImage target = {device(), img, view};

	return target;
}

void ViewApp::initBuffers(const vk::Extent2D& size,
		nytl::Span<RenderBuffer> bufs) {
	auto& dev = vulkanDevice();
	auto scPos = 0u; // attachments[scPos]: swapchain image
	depthTarget() = createDepthTarget(size);

	std::vector<vk::ImageView> attachments;
	auto initBuf = [&](vpp::ViewableImage& img, vk::Format format) {
		auto usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::inputAttachment |
			vk::ImageUsageBits::transferSrc |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo::color(device(),
			{size.width, size.height}, usage, {format},
			vk::ImageTiling::optimal, samples());
		dlg_assert(info);
		img = {device(), *info};
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
	auto info = vpp::ViewableImageCreateInfo::color(device(),
		{size.width, size.height}, usage, {emissionFormat},
		vk::ImageTiling::optimal, samples());
	dlg_assert(info);
	auto bloomLevels = std::min(maxBloomLevels, doi::mipmapLevels(size));
	info->img.mipLevels = 1 + bloomLevels;
	gpass_.emission = {device(), *info};

	attachments.push_back(gpass_.emission.vkImageView());
	attachments.push_back(depthTarget().vkImageView()); // depth

	depthMipLevels_ = doi::mipmapLevels(size);
	// depthMipLevels_ = 1u;
	usage = vk::ImageUsageBits::colorAttachment |
		vk::ImageUsageBits::inputAttachment |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst |
		vk::ImageUsageBits::sampled;
	info = vpp::ViewableImageCreateInfo::color(device(),
		{size.width, size.height}, usage, {ldepthFormat},
		vk::ImageTiling::optimal, samples());
	dlg_assert(info);
	info->img.mipLevels = depthMipLevels_;
	gpass_.depth = {device(), *info};
	attachments.push_back(gpass_.depth.vkImageView());

	info->view.subresourceRange.levelCount = depthMipLevels_;
	info->view.image = gpass_.depth.image();
	depthMipView_ = {device(), info->view};

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

	initBuf(lpass_.light, lightFormat);
	fbi = vk::FramebufferCreateInfo({}, lpass_.pass,
		attachments.size(), attachments.data(),
		size.width, size.height, 1);
	lpass_.fb = {dev, fbi};

	// ssao buf
	attachments.clear();
	initBuf(ssao_.target, ssaoFormat);
	attachments.push_back(gpass_.normal.vkImageView());
	fbi = vk::FramebufferCreateInfo({}, ssao_.pass,
		attachments.size(), attachments.data(),
		size.width, size.height, 1);
	ssao_.fb = {dev, fbi};

	// scatter buf
	attachments.clear();
	initBuf(scatter_.target, scatterFormat);
	fbi = vk::FramebufferCreateInfo({}, scatter_.pass,
		attachments.size(), attachments.data(),
		size.width, size.height, 1);
	scatter_.fb = {dev, fbi};

	// TODO: really start with half-sized bloom for first level?
	// try out different settings/make it confiurable. Should have
	// major impact on visuals *and* performance
	usage = vk::ImageUsageBits::storage | vk::ImageUsageBits::sampled;
	info = vpp::ViewableImageCreateInfo::color(device(),
		{size.width / 2, size.height / 2}, usage, {emissionFormat},
		vk::ImageTiling::optimal, samples());
	info->img.mipLevels = bloomLevels;
	dlg_assert(info);
	bloom_.tmpTarget = {device(), info->img};

	pp_.params.bloomLevels = bloomLevels;
	pp_.updateParams = true;
	bloom_.emissionLevels.clear();
	bloom_.tmpLevels.clear();
	for(auto i = 0u; i < bloomLevels; ++i) {
		auto ivi = info->view;

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

	auto ivi = info->view;
	ivi.image = gpass_.emission.image();
	ivi.subresourceRange.baseMipLevel = 0;
	ivi.subresourceRange.levelCount = bloomLevels;
	bloom_.fullBloom = {dev, ivi};

	// ssr target image
	usage = vk::ImageUsageBits::storage | vk::ImageUsageBits::sampled;
	info = vpp::ViewableImageCreateInfo::color(device(),
		{size.width, size.height}, usage, {ssrFormat},
		vk::ImageTiling::optimal, samples());
	dlg_assert(info);
	ssr_.target = {device(), *info};

	// create swapchain framebuffers
	attachments.clear();
	attachments.emplace_back(); // scPos
	// attachments.push_back(lpass_.light.vkImageView());
	attachments.push_back(gpass_.albedo.vkImageView());

	for(auto& buf : bufs) {
		attachments[scPos] = buf.imageView;
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
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ldsu.inputAttachment({{{}, depthTarget().imageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});

	auto needDepth = flagScattering | flagSSR | flagSSAO;
	auto ssaoView = (pp_.params.flags & flagSSAO) ?
		ssao_.target.vkImageView() : dummyTex_.vkImageView();
	auto scatterView = (pp_.params.flags & flagScattering) ?
		scatter_.target.vkImageView() : dummyTex_.vkImageView();
	auto depthView = ((pp_.params.flags & needDepth) != 0) ?
		depthMipView_ : gpass_.depth.vkImageView();
	auto bloomView = (pp_.params.flags & flagBloom) ?
		bloom_.fullBloom : gpass_.emission.vkImageView();
	auto ssrView = (pp_.params.flags & flagSSR) ?
		ssr_.target.vkImageView() : dummyTex_.vkImageView();

	auto& pdsu = updates.emplace_back(pp_.ds);
	// ldsu.inputAttachment({{{}, lpass_.light.vkImageView(),
	// 	vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, lpass_.light.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.inputAttachment({{{}, gpass_.albedo.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.uniform({{pp_.ubo.buffer(), pp_.ubo.offset(), pp_.ubo.size()}});
	pdsu.imageSampler({{{}, ssaoView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, depthView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, scatterView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, gpass_.normal.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, bloomView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	pdsu.imageSampler({{{}, ssrView,
		vk::ImageLayout::shaderReadOnlyOptimal}});

	auto& ssdsu = updates.emplace_back(ssao_.ds);
	ssdsu.uniform({{ssao_.samples.buffer(), ssao_.samples.offset(),
		ssao_.samples.size()}});
	ssdsu.imageSampler({{{}, ssao_.noise.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ssdsu.imageSampler({{{}, depthView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ssdsu.inputAttachment({{{}, gpass_.normal.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});

	auto& sdsu = updates.emplace_back(scatter_.ds);
	sdsu.imageSampler({{{}, depthView,
		vk::ImageLayout::shaderReadOnlyOptimal}});

	if(pp_.params.flags & flagBloom) {
		for(auto i = 0u; i < pp_.params.bloomLevels; ++i) {
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

	auto& rdsu = updates.emplace_back(ssr_.ds);
	rdsu.imageSampler({{{}, depthView,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	rdsu.imageSampler({{{}, gpass_.normal.vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	rdsu.storage({{{}, ssr_.target.vkImageView(),
		vk::ImageLayout::general}});

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

	// render shadow maps
	for(auto& light : pointLights_) {
		light.render(cb, shadowData_, *scene_);
	}
	for(auto& light : dirLights_) {
		light.render(cb, shadowData_, *scene_);
	}

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
			gpass_.pipeLayout, 0, {sceneDs_}, {});
		scene_->render(cb, gpass_.pipeLayout);

		// NOTE: ideally, don't render these in gbuffer pass but later
		// on with different lightning?
		// for(auto& l : pointLights_) {
		// 	l.lightBall().render(cb, gPipeLayout_);
		// }
		// for(auto& l : dirLights_) {
		// 	l.lightBall().render(cb, gPipeLayout_);
		// }

		vk::cmdEndRenderPass(cb);
	}

	// create depth mipmaps
	auto needDepth = flagScattering | flagSSR | flagSSAO;
	if(depthMipLevels_ > 1 && (pp_.params.flags & needDepth) != 0) {
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
				depthImg, vk::ImageLayout::transferDstOptimal, {blit},
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

	// ssao
	if(pp_.params.flags & flagSSAO) {
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
			ssao_.pipeLayout, 0, {sceneDs_, ssao_.ds}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);
	}

	// scatter
	if(pp_.params.flags & flagScattering) {
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
			scatter_.pipeLayout, 0, {sceneDs_, scatter_.ds, lds}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);
	}

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
			lpass_.pipeLayout, 0, {sceneDs_, lpass_.ds}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, lpass_.pointPipe);
		for(auto& light : pointLights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				lpass_.pipeLayout, 2, {light.ds()}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		}

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, lpass_.dirPipe);
		for(auto& light : dirLights_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				lpass_.pipeLayout, 2, {light.ds()}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		}

		vk::cmdEndRenderPass(cb);
	}

	// bloom
	if(pp_.params.flags & flagBloom) {
		// create emission mipmaps
		auto& emission = gpass_.emission.image();
		auto bloomLevels = pp_.params.bloomLevels;

		// transition mipmap level 0 transferSrc
		vpp::changeLayout(cb, emission,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::allCommands,
			vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite,
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

		// transition all but mipmap level 0 to transferDst
		vpp::changeLayout(cb, emission,
			vk::ImageLayout::undefined, // we don't need the content any more
			vk::PipelineStageBits::topOfPipe, {},
			vk::ImageLayout::transferDstOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferWrite,
			{vk::ImageAspectBits::color, 1, bloomLevels, 0, 1});

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
				emission, vk::ImageLayout::transferDstOptimal, {blit},
				vk::Filter::linear);

			// change layout of current mip level to transferSrc for next mip level
			vpp::changeLayout(cb, emission,
				vk::ImageLayout::transferDstOptimal,
				vk::PipelineStageBits::transfer,
				vk::AccessBits::transferWrite,
				vk::ImageLayout::transferSrcOptimal,
				vk::PipelineStageBits::transfer,
				vk::AccessBits::transferRead,
				{vk::ImageAspectBits::color, i, 1, 0, 1});
		}

		// transform all levels back to readonly for blur read pass
		vpp::changeLayout(cb, emission,
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::allCommands,
			vk::AccessBits::shaderRead,
			{vk::ImageAspectBits::color, 0, bloomLevels + 1, 0, 1});

		// make tmp target writable
		vpp::changeLayout(cb, bloom_.tmpTarget,
			vk::ImageLayout::undefined,
			vk::PipelineStageBits::allCommands,
			vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite,
			vk::ImageLayout::general,
			vk::PipelineStageBits::computeShader,
			vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite,
			{vk::ImageAspectBits::color, 0, bloomLevels, 0, 1});

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
				bloom_.pipeLayout, 0, {tmp.ds}, {});

			auto w = std::max(width >> (i + 1), 1u);
			auto h = std::max(height >> (i + 1), 1u);
			vk::cmdDispatch(cb, std::ceil(w / 32.f), std::ceil(h / 32.f), 1);

			vpp::changeLayout(cb, bloom_.tmpTarget,
				vk::ImageLayout::general,
				vk::PipelineStageBits::allCommands,
				vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite,
				vk::ImageLayout::shaderReadOnlyOptimal,
				vk::PipelineStageBits::computeShader |
					vk::PipelineStageBits::fragmentShader,
				vk::AccessBits::shaderRead,
				{vk::ImageAspectBits::color, i, 1, 0, 1});
			vpp::changeLayout(cb, emissionImg,
				vk::ImageLayout::undefined,
				vk::PipelineStageBits::allCommands,
				vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite,
				vk::ImageLayout::general,
				vk::PipelineStageBits::computeShader,
				vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite,
				{vk::ImageAspectBits::color, i + 1, 1, 0, 1});

			horizontal = 0u;
			vk::cmdPushConstants(cb, bloom_.pipeLayout, vk::ShaderStageBits::compute,
				0, 4, &horizontal);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
				bloom_.pipeLayout, 0, {emission.ds}, {});
			vk::cmdDispatch(cb, std::ceil(w / 32.f), std::ceil(h / 32.f), 1);
		}

		// make all meission layers readonly
		vpp::changeLayout(cb, emission,
			vk::ImageLayout::general,
			vk::PipelineStageBits::allCommands,
			vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::allCommands,
			vk::AccessBits::shaderRead,
			{vk::ImageAspectBits::color, 1, bloomLevels, 0, 1});
	}

	// ssr pass
	{
		auto target = ssr_.target.vkImage();
		vpp::changeLayout(cb, target,
			vk::ImageLayout::undefined, // don't need that anymore
			vk::PipelineStageBits::topOfPipe, {},
			vk::ImageLayout::general,
			vk::PipelineStageBits::computeShader,
			vk::AccessBits::shaderWrite,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute,
			ssr_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			ssr_.pipeLayout, 0, {sceneDs_, ssr_.ds}, {});
		vk::cmdDispatch(cb, std::ceil(width / 8.f), std::ceil(height / 8.f), 1);

		vpp::changeLayout(cb, target,
			vk::ImageLayout::general,
			vk::PipelineStageBits::computeShader,
			vk::AccessBits::shaderWrite,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::fragmentShader,
			vk::AccessBits::shaderRead,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});
	}

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

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pp_.pipeLayout, 0, {sceneDs_, pp_.ds}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen

		// gui stuff
		rvgContext().bindDefaults(cb);
		gui().draw(cb);

		vk::cmdEndRenderPass(cb);
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

	auto update = camera_.update || updateLights_ || selectionPos_;
	update |= selectionPending_ || pp_.updateParams;
	if(update) {
		App::scheduleRedraw();
	}

	// TODO: only here for fps testing
	App::scheduleRedraw();
}

bool ViewApp::key(const ny::KeyEvent& ev) {
	if(App::key(ev)) {
		return true;
	}

	if(!ev.pressed) {
		return false;
	}

	auto numModes = 10u;
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
			showMode_ = (showMode_ + 1) % numModes;
			camera_.update = true; // trigger update device
			return true;
		case ny::Keycode::l:
			showMode_ = (showMode_ + numModes - 1) % numModes;
			camera_.update = true; // trigger update device
			return true;
		case ny::Keycode::i:
			pp_.params.aoFactor = 1.f - pp_.params.aoFactor;
			pp_.updateParams = true;
			return true;
		case ny::Keycode::equals:
			pp_.params.exposure *= 1.1f;
			pp_.updateParams = true;
			return true;
		case ny::Keycode::minus:
			pp_.params.exposure /= 1.1f;
			pp_.updateParams = true;
			return true;
		default:
			break;
	}

	auto uk = static_cast<unsigned>(ev.keycode);
	auto k1 = static_cast<unsigned>(ny::Keycode::k1);
	auto k0 = static_cast<unsigned>(ny::Keycode::k0);
	if(uk >= k1 && uk <= k0) {
		auto diff = (1 + uk - k1) % numModes;
		showMode_ = diff;
		camera_.update = true;
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
		doi::write(span, showMode_);

		// skybox_.updateDevice(fixedMatrix(camera_));

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

	if(pp_.updateParams) {
		auto map = pp_.ubo.memoryMap();
		auto span = map.span();
		doi::write(span, pp_.params);
	}

	if(selectionPending_) {
		auto bytes = vpp::retrieveMap(selectionStage_,
			vk::Format::r32g32b32a32Sfloat,
			{1u, 1u, 1u}, {vk::ImageAspectBits::color});
		dlg_assert(bytes.size() == sizeof(float) * 4);
		auto fp = reinterpret_cast<const float*>(bytes.data());
		auto selected = 65536 * (0.5f + 0.5f * fp[2]);
		dlg_info("selected: {} ({} {} {} {})", selected,
			fp[0], fp[1], fp[2], fp[3]);
		selectionPending_ = {};
	}

	// recevie selection id
	// read the one pixel from the normals buffer (containing the primitive id)
	// where the mouse was pressed
	if(selectionPos_) {
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
			{blit}, vk::Filter::nearest);
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
	}

	if(updateDescriptors_) {
		updateDescriptors();
	}
}

void ViewApp::resize(const ny::SizeEvent& ev) {
	App::resize(ev);
	selectionPos_ = {};
	camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
	camera_.update = true;
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
	if(!app.init({*argv, std::size_t(argc)})) {
		return EXIT_FAILURE;
	}

	app.run();
}
