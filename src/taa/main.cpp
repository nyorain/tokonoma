#include <tkn/passes/taa.hpp>
#include <tkn/ccam.hpp>
#include <tkn/app.hpp>
#include <tkn/render.hpp>
#include <tkn/features.hpp>
#include <tkn/formats.hpp>
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

#include <swa/swa.h>

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
#include <nytl/stringParam.hpp>

#include <vui/gui.hpp>
#include <vui/dat.hpp>

#include <tinygltf.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/taa.model.vert.h>
#include <shaders/taa.model.frag.h>
#include <shaders/taa.pp.frag.h>
// #include <shaders/taa.taa.comp.h>

#include <optional>
#include <vector>
#include <string>
#include <array>

// NOTE: this used to implement a temporal super sampling pass but that was
// moved to tkn (see tkn/taa.hpp) since other projects use it now as well.
// It still showcases that pass.

using namespace tkn::types;

class ViewApp : public tkn::App {
public:
	static constexpr auto historyFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto offscreenFormat = vk::Format::r16g16b16a16Sfloat;
	// TODO: we can probably use a 8-bit buffer here with a proper encoding
	static constexpr auto velocityFormat = vk::Format::r16g16b16a16Sfloat;

	static constexpr u32 modeDirLight = (1u << 0);
	static constexpr u32 modePointLight = (1u << 1);
	static constexpr u32 modeSpecularIBL = (1u << 2);
	static constexpr u32 modeIrradiance = (1u << 3);

	struct Args : public App::Args {
		std::string model;
		float scale {1.f};
	};

public:
	bool init(nytl::Span<const char*> cargs) override {
		Args args;
		if(!tkn::App::doInit(cargs, args)) {
			return false;
		}

		camera_.near(-0.1f);
		camera_.far(-8.f);

		auto& dev = vkDevice();
		auto hostMem = dev.hostMemoryTypes();
		depthFormat_ = tkn::findDepthFormat(dev);

		vpp::DeviceMemoryAllocator memStage(dev);
		vpp::BufferAllocator bufStage(memStage);

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		auto wb = tkn::WorkBatcher(dev);
		wb.cb = cb;

		auto initBrdfLut = tkn::createTexture(wb, tkn::loadImage("brdflut.ktx"));
		brdfLut_ = tkn::initTexture(initBrdfLut, wb);

		// tex sampler
		vk::SamplerCreateInfo sci {};
		sci.addressModeU = vk::SamplerAddressMode::clampToEdge;
		sci.addressModeV = vk::SamplerAddressMode::clampToEdge;
		sci.addressModeW = vk::SamplerAddressMode::clampToEdge;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.mipmapMode = vk::SamplerMipmapMode::linear;
		sci.minLod = 0.0;
		sci.maxLod = 100;
		linearSampler_ = {dev, sci};

		sci.magFilter = vk::Filter::nearest;
		sci.minFilter = vk::Filter::nearest;
		sci.mipmapMode = vk::SamplerMipmapMode::nearest;
		nearestSampler_ = {dev, sci};

		auto idata = std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u};
		auto span = nytl::as_bytes(nytl::span(idata));
		auto p = tkn::wrapImage({1u, 1u, 1u}, vk::Format::r8g8b8a8Unorm, span);
		tkn::TextureCreateParams params;
		params.format = vk::Format::r8g8b8a8Unorm;

		auto initDummyTex = tkn::createTexture(wb, std::move(p), params);
		dummyTex_ = tkn::initTexture(initDummyTex, wb);

		// Load scene
		auto mat = tkn::scaleMat<4, float>(nytl::Vec3f{args.scale, args.scale, args.scale});
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
			throw std::runtime_error("Couldn't load gltf file");
		}

		auto& model = *omodel;
		auto scene = model.defaultScene >= 0 ? model.defaultScene : 0;
		auto& sc = model.scenes[scene];

		// NOTE: this is needed because we always want to sampler from
		// a higher miplevel than the one that would usually be selected.
		// Increases sharpness, we anti-aliase it anyways via jittering.
		static constexpr auto mipLodBias = -1.f;
		auto initScene = vpp::InitObject<tkn::Scene>(scene_, wb, path,
			model, sc, mat, ri, mipLodBias);
		initScene.init(wb, dummyTex_.vkImageView());

		tkn::initShadowData(shadowData_, dev, depthFormat_,
			scene_.dsLayout(), multiview_, depthClamp_);

		// view + projection matrix
		auto cameraBindings = std::array {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		cameraDsLayout_.init(dev, cameraBindings);

		// ao
		auto sampler = &linearSampler_.vkHandle();
		auto aoBindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, sampler),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, sampler),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, sampler),
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

		// offscreen render pass
		auto dstScope = taa_.dstScopeInput();
		std::array<vk::AttachmentDescription, 3> attachments;
		attachments[0].initialLayout = vk::ImageLayout::undefined;
		attachments[0].finalLayout = dstScope.layout;
		attachments[0].format = offscreenFormat;
		attachments[0].loadOp = vk::AttachmentLoadOp::clear;
		attachments[0].storeOp = vk::AttachmentStoreOp::store;
		attachments[0].samples = vk::SampleCountBits::e1;

		attachments[1].initialLayout = vk::ImageLayout::undefined;
		attachments[1].finalLayout = dstScope.layout;
		attachments[1].format = depthFormat_;
		attachments[1].loadOp = vk::AttachmentLoadOp::clear;
		attachments[1].storeOp = vk::AttachmentStoreOp::store;
		attachments[1].samples = vk::SampleCountBits::e1;

		attachments[2].initialLayout = vk::ImageLayout::undefined;
		attachments[2].finalLayout = dstScope.layout;
		attachments[2].format = velocityFormat;
		attachments[2].loadOp = vk::AttachmentLoadOp::clear;
		attachments[2].storeOp = vk::AttachmentStoreOp::store;
		attachments[2].samples = vk::SampleCountBits::e1;

		vk::AttachmentReference colorRefs[2];
		colorRefs[0].attachment = 0;
		colorRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;
		colorRefs[1].attachment = 2;
		colorRefs[1].layout = vk::ImageLayout::colorAttachmentOptimal;

		vk::AttachmentReference depthRef;
		depthRef.attachment = 1;
		depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.colorAttachmentCount = 2;
		subpass.pColorAttachments = colorRefs;
		subpass.pDepthStencilAttachment = &depthRef;

		// make sure color and depth buffer are available afterwards
		// in taa compute shader
		vk::SubpassDependency dependency;
		dependency.srcSubpass = 0;
		dependency.srcStageMask =
			vk::PipelineStageBits::colorAttachmentOutput |
			vk::PipelineStageBits::earlyFragmentTests |
			vk::PipelineStageBits::lateFragmentTests;
		dependency.srcAccessMask =
			vk::AccessBits::colorAttachmentWrite |
			vk::AccessBits::depthStencilAttachmentWrite;
		dependency.dstSubpass = vk::subpassExternal;
		dependency.dstStageMask = dstScope.stages;
		dependency.dstAccessMask = dstScope.access;

		vk::RenderPassCreateInfo rpi;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &subpass;
		rpi.attachmentCount = attachments.size();
		rpi.pAttachments = attachments.data();
		rpi.dependencyCount = 1u;
		rpi.pDependencies = &dependency;
		offscreen_.rp = {dev, rpi};

		// pipeline
		vpp::ShaderModule vertShader(dev, taa_model_vert_data);
		vpp::ShaderModule fragShader(dev, taa_model_frag_data);
		vpp::GraphicsPipelineInfo gpi {offscreen_.rp, pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}};

		gpi.vertex = tkn::Scene::vertexInfo();
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		auto battachments = {
			tkn::noBlendAttachment(),
			tkn::noBlendAttachment(),
		};
		gpi.blend.attachmentCount = battachments.size();
		gpi.blend.pAttachments = battachments.begin();

		// NOTE: see the gltf material.doubleSided property. We can't switch
		// this per material (without requiring two pipelines) so we simply
		// always render backfaces currently and then dynamically cull in the
		// fragment shader. That is required since some models rely on
		// backface culling for effects (e.g. outlines). See model.frag
		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		pipe_ = {dev, gpi.info()};

		// gpi.depthStencil.depthWriteEnable = false;
		// blendPipe_ = {dev, gpi.info()};

		// we need two blend attachments since this render pass has two
		// color attachments (color + velocity)
		// but the environment should simply ignore the velocity buffer
		auto batts = {
			tkn::defaultBlendAttachment(),
			tkn::disableBlendAttachment(),
		};

		tkn::Environment::InitData initEnv;
		env_.create(initEnv, wb, "convolution.ktx", "irradiance.ktx", linearSampler_);
		env_.createPipe(dev, cameraDsLayout_, offscreen_.rp, 0u,
			vk::SampleCountBits::e1, batts);
		env_.init(initEnv, wb);

		// camera
		auto cameraUboSize = sizeof(nytl::Mat4f) * 2 // proj matrix (+last)
			+ sizeof(nytl::Vec2f) // jitter offset
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
		dirLight_.data.color = {5.f, 5.f, 5.f};

		pointLight_ = {wb, shadowData_};
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

		auto sm = nytl::identity<4, float>();
		movingInstance_ = scene_.addInstance(cubePrimitiveID_, sm,
			scene_.defaultMaterialID());

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

		initPP();
		taa_.init(dev, linearSampler_, nearestSampler_);

		// init layouts cb
		semaphoreInitLayouts_ = {dev};
		auto qf = dev.queueSubmitter().queue().family();
		cbInitLayouts_ = dev.commandAllocator().get(qf,
			vk::CommandPoolCreateBits::resetCommandBuffer);

		// create gui
		rvgInit(pp_.rp, 0, vk::SampleCountBits::e1);
		auto& gui = this->gui();

		using namespace vui::dat;
		auto pos = nytl::Vec2f {100.f, 0};
		auto& panel = gui.create<vui::dat::Panel>(pos, 300.f);

		// from deferred/main.cpp
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

		createValueTextfield(panel, "minFac", taa_.params.minFac);
		createValueTextfield(panel, "maxFac", taa_.params.maxFac);
		createValueTextfield(panel, "velWeight", taa_.params.velWeight);

		createValueTextfield(panel, "exposure", pp_.exposure);
		createValueTextfield(panel, "sharpen", pp_.sharpen);

		auto createFlagCheckbox = [&](auto& at, auto name, auto& flags,
				auto flag, bool* set = {}) {
			auto& cb = at.template create<Checkbox>(name).checkbox();
			cb.set(u32(flags) & u32(flag));
			cb.onToggle = [&flags, flag, set](auto&) {
				flags ^= flag;
				if(set) *set = true;
			};
			return &cb;
		};

		createFlagCheckbox(panel, "depthReject", taa_.params.flags,
			tkn::TAAPass::flagDepthReject);
		createFlagCheckbox(panel, "closestDepth", taa_.params.flags,
			tkn::TAAPass::flagClosestDepth);
		createFlagCheckbox(panel, "tonemap", taa_.params.flags,
			tkn::TAAPass::flagTonemap);
		createFlagCheckbox(panel, "colorClip", taa_.params.flags,
			tkn::TAAPass::flagColorClip);
		createFlagCheckbox(panel, "luminanceWeigh", taa_.params.flags,
			tkn::TAAPass::flagLuminanceWeigh);
		createFlagCheckbox(panel, "bicubic", taa_.params.flags,
			tkn::TAAPass::flagBicubic);

		auto& cbox = panel.create<Checkbox>("disable").checkbox();
		cbox.onToggle = [&](auto&) {
			disable_ ^= true;
			taa_.params.flags ^= tkn::TAAPass::flagPassthrough;
		};

		return true;
	}

	void initPP() {
		auto& dev = vkDevice();

		vk::AttachmentDescription attachment;
		attachment.initialLayout = vk::ImageLayout::undefined;
		attachment.finalLayout = vk::ImageLayout::presentSrcKHR;
		attachment.format = swapchainInfo().imageFormat;
		// no clear needed, we render fullscreen
		// basically just a copy
		attachment.loadOp = vk::AttachmentLoadOp::dontCare;
		attachment.storeOp = vk::AttachmentStoreOp::store;
		attachment.samples = vk::SampleCountBits::e1;

		vk::AttachmentReference colorRef;
		colorRef.attachment = 0;
		colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		vk::RenderPassCreateInfo rpi;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &subpass;
		rpi.attachmentCount = 1;
		rpi.pAttachments = &attachment;
		pp_.rp = {dev, rpi};

		auto ppBindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &nearestSampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment)
		};

		pp_.dsLayout.init(dev, ppBindings);
		pp_.pipeLayout = {dev, {{pp_.dsLayout.vkHandle()}}, {}};
		pp_.ds = {dev.descriptorAllocator(), pp_.dsLayout};

		vpp::ShaderModule vertShader(dev, tkn_fullscreen_vert_data);
		vpp::ShaderModule fragShader(dev, taa_pp_frag_data);
		vpp::GraphicsPipelineInfo gpi {pp_.rp, pp_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}};

		auto atts = {tkn::noBlendAttachment()};
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.attachmentCount = atts.size();
		gpi.blend.pAttachments = atts.begin();
		pp_.pipe = {dev, gpi.info()};

		auto uboSize = sizeof(float) * 2;
		pp_.ubo = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
	}

	void initBuffers(const vk::Extent2D& size,
			nytl::Span<RenderBuffer> bufs) override {
		auto& dev = vkDevice();

		auto depthUsage = vk::ImageUsageBits::depthStencilAttachment |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo(depthFormat_,
			vk::ImageAspectBits::depth, size, depthUsage);
		offscreen_.depthTarget = {dev.devMemAllocator(),
			info, dev.deviceMemoryTypes()};

		for(auto& buf : bufs) {
			vk::FramebufferCreateInfo fbi({},
				pp_.rp, 1u, &buf.imageView.vkHandle(),
				size.width, size.height, 1);
			buf.framebuffer = {dev, fbi};
		}

		info = vpp::ViewableImageCreateInfo(offscreenFormat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::sampled);
		dlg_assert(vpp::supported(dev, info.img));
		offscreen_.target = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		info.img.format = velocityFormat;
		info.view.format = velocityFormat;
		dlg_assert(vpp::supported(dev, info.img));
		offscreen_.velTarget = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		auto attachments = {
			offscreen_.target.vkImageView(),
			offscreen_.depthTarget.vkImageView(),
			offscreen_.velTarget.vkImageView(),
		};
		auto fbi = vk::FramebufferCreateInfo({}, offscreen_.rp,
			attachments.size(), attachments.begin(),
			size.width, size.height, 1);
		offscreen_.fb = {dev, fbi};

		// descriptors
		taa_.initBuffers(size,
			offscreen_.target.vkImageView(),
			offscreen_.depthTarget.vkImageView(),
			offscreen_.velTarget.vkImageView());

		vpp::DescriptorSetUpdate pdsu(pp_.ds);
		// we can use inHistory here because the copy the data between
		// the compute shader and the post processing shader from
		// outHistory back to inHistory
		pdsu.imageSampler({{{}, taa_.targetView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		pdsu.uniform({{{pp_.ubo}}});
	}

	bool features(tkn::Features& enable,
			const tkn::Features& supported) override {
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

	bool handleArgs(const argagg::parser_results& result,
			App::Args& bout) override {
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

		return true;
	}

	void record(const RenderBuffer& buf) override {
		auto cb = buf.commandBuffer;
		vk::beginCommandBuffer(cb, {});

		if(mode_ & modeDirLight) {
			dirLight_.render(cb, shadowData_, scene_);
		}
		if(mode_ & modePointLight) {
			pointLight_.render(cb, shadowData_, scene_);
		}

		auto [width, height] = swapchainInfo().imageExtent;
		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		// 1: render offscreeen
		std::array<vk::ClearValue, 3u> cv {};
		cv[0] = {{0.f, 0.f, 0.f, 0.f}}; // color
		cv[1].depthStencil = {1.f, 0u}; // depth
		cv[2] = {{0.f, 0.f, 0.f, 0.f}}; // velocity
		vk::cmdBeginRenderPass(cb, {offscreen_.rp, offscreen_.fb,
			{0u, 0u, width, height},
			std::uint32_t(cv.size()), cv.data()}, {});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, false); // opaque

		tkn::cmdBindGraphicsDescriptors(cb, env_.pipeLayout(), 0,
			{envCameraDs_});
		env_.render(cb);

		// vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, blendPipe_);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, true); // transparent/blend

		vk::cmdEndRenderPass(cb);

		// 2: apply TAA
		taa_.record(cb, {width, height});
		tkn::barrier(cb, taa_.targetImage(), taa_.srcScope(),
			tkn::SyncScope::fragmentSampled());

		// 3: post processing, apply to swapchain buffer
		vk::cmdBeginRenderPass(cb, {pp_.rp, buf.framebuffer,
			{0u, 0u, width, height}, 0u, nullptr}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, pp_.pipeLayout, 0, {pp_.ds});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad triangle fan

		// gui stuff
		rvgContext().bindDefaults(cb);
		gui().draw(cb);

		vk::cmdEndRenderPass(cb);
		vk::endCommandBuffer(cb);
	}

	void update(double dt) override {
		App::update(dt);
		time_ += dt;

		// movement
		camera_.update(swaDisplay(), dt);

		// animate instance (simple moving in x dir)
		// movingVel_ *= std::pow(0.99, dt);
		movingVel_.x += 0.1 * dt * std::cos(0.5 * time_);
		movingPos_ += dt * movingVel_;
		auto mat = tkn::translateMat(movingPos_) * tkn::scaleMat(nytl::Vec3f{2.f, 2.f, 2.f});
		auto& ini = scene_.instances()[movingInstance_];
		ini.lastMatrix = ini.matrix;
		ini.matrix = mat;
		scene_.updatedInstance(movingInstance_);

		// we currently always redraw to see consistent fps
		App::scheduleRedraw();
	}

	bool key(const swa_key_event& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		switch(ev.keycode) {
			case swa_key_m: { // move light here
				dirLight_.data.dir = -camera_.position();
				auto& ini = scene_.instances()[dirLight_.instanceID];
				ini.lastMatrix = ini.matrix = dirLightObjMatrix();
				scene_.updatedInstance(dirLight_.instanceID);
				updateLight_ = true;
				return true;
			} case swa_key_n: { // move light here
				updateLight_ = true;
				pointLight_.data.position = camera_.position();
				auto& ini = scene_.instances()[pointLight_.instanceID];
				ini.lastMatrix = ini.matrix = pointLightObjMatrix();
				scene_.updatedInstance(pointLight_.instanceID);
				return true;
			} case swa_key_p:
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
				App::scheduleRerecord();
				dlg_info("dir light: {}", bool(mode_ & modeDirLight));
				return true;
			case swa_key_k2:
				mode_ ^= modePointLight;
				updateLight_ = true;
				updateAOParams_ = true;
				App::scheduleRerecord();
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
		App::mouseMove(ev);
		camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool mouseWheel(float dx, float dy) override {
		if(App::mouseWheel(dx, dy)) {
			return true;
		}

		camera_.mouseWheel(dy);
		return true;
	}

	void updateDevice() override {
		App::updateDevice();
		updateLight_ |= camera_.needsUpdate; // for directional light

		auto vp = camera_.viewProjectionMatrix();

		// always update the camera matrix
		// update scene ubo
		{
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			// sample sequence from
			// community.arm.com/developer/tools-software/graphics/b/blog/posts/temporal-anti-aliasing
			auto [width, height] = swapchainInfo().imageExtent;

			using namespace nytl::vec::cw::operators;
			auto sample = disable_ ? nytl::Vec2f{0.f, 0.f} : taa_.nextSample();
			auto pixSize = Vec2f{1.f / width, 1.f / height};
			auto off = sample * pixSize;

			tkn::write(span, vp);
			tkn::write(span, lastVP_);
			tkn::write(span, off);
			tkn::write(span, -camera_.near());
			tkn::write(span, -camera_.far());
			tkn::write(span, camera_.position());
			camera_.needsUpdate = false;
			map.flush();

			lastVP_ = vp;

			auto envMap = envCameraUbo_.memoryMap();
			auto envSpan = envMap.span();
			tkn::write(envSpan, camera_.fixedViewProjectionMatrix());
			envMap.flush();

		}

		taa_.updateDevice(-camera_.near(), -camera_.far());

		auto semaphore = scene_.updateDevice(vp);
		if(semaphore) {
			addSemaphore(semaphore, vk::PipelineStageBits::allGraphics);
			App::scheduleRerecord();
		}

		if(updateLight_) {
			if(mode_ & modeDirLight) {
				dirLight_.updateDevice(vp, -camera_.near(), -camera_.far());

				// NOTE: tight view frustum just for shadow mapping
				// should be easier than this...
				// auto lnear = -0.2f;
				// auto lfar = -4.f;
				// auto on = camera_.near();
				// auto of = camera_.far();
				// camera_.near(lnear);
				// camera_.far(lfar);
				// dirLight_.updateDevice(camera_.viewProjectionMatrix(), -lnear, -lfar);
				// camera_.near(on);
				// camera_.far(of);
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

		auto map = pp_.ubo.memoryMap();
		auto span = map.span();
		tkn::write(span, pp_.sharpen);
		tkn::write(span, pp_.exposure);
		map.flush();
	}

	// only for visualizing the box/sphere
	nytl::Mat4f dirLightObjMatrix() {
		return tkn::translateMat(-dirLight_.data.dir);
	}

	nytl::Mat4f pointLightObjMatrix() {
		return tkn::translateMat(pointLight_.data.position);
	}

	void resize(unsigned w, unsigned h) override {
		App::resize(w, h);
		camera_.aspect({w, h});
	}

	const char* name() const override { return "Temporal Anti aliasing"; }

protected:
	vk::Format depthFormat_;
	vpp::Sampler linearSampler_;
	vpp::Sampler nearestSampler_;
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
	float aoFac_ {0.05f};

	u32 mode_ {modePointLight | modeIrradiance | modeSpecularIBL};

	vpp::ViewableImage dummyTex_;

	struct {
		vpp::RenderPass rp;
		vpp::ViewableImage target;
		vpp::ViewableImage velTarget;
		vpp::ViewableImage depthTarget;
		vpp::Framebuffer fb;
	} offscreen_;

	tkn::TAAPass taa_;
	nytl::Mat4f lastVP_ = nytl::identity<4, float>();
	bool disable_ {false};

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::RenderPass rp;
		vpp::SubBuffer ubo;

		float sharpen {0.0};
		float exposure {1.0};
	} pp_;

	vpp::Semaphore semaphoreInitLayouts_ {};
	vpp::CommandBuffer cbInitLayouts_ {};

	bool anisotropy_ {}; // whether device supports anisotropy
	bool multiview_ {}; // whether device supports multiview
	bool depthClamp_ {}; // whether the device supports depth clamping
	bool multiDrawIndirect_ {};

	float time_ {};

	tkn::Scene scene_;
	tkn::ControlledCamera camera_ {};

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

	u32 movingInstance_ {};
	nytl::Vec3f movingPos_ {0.f, 1.f, 0.f};
	nytl::Vec3f movingVel_ {0, 0, 0};

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
