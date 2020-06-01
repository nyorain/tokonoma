#include <tkn/camera.hpp>
#include <tkn/app.hpp>
#include <tkn/render.hpp>
#include <tkn/window.hpp>
#include <tkn/transform.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/gltf.hpp>
#include <tkn/quaternion.hpp>
#include <tkn/scene/shape.hpp>
#include <tkn/scene/scene.hpp>
#include <tkn/scene/scene.hpp>
#include <tkn/scene/environment.hpp>
#include <argagg.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>
#include <ny/mouseButton.hpp>

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

#include <tinygltf.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/repro.model.vert.h>
#include <shaders/repro.model.frag.h>
#include <shaders/repro.snap.frag.h>
#include <shaders/repro.pp.frag.h>

#include <optional>
#include <vector>
#include <string>

// ideas:
// - use depth bounds (and other stuff) as effects
//   like "focusing on a certain distance", dof equivalent for
//   the repro sensory system
// - use a projection matrix that uses linear depth when taking
//   the snapshot? not sure yet
// - maybe make the whole "repro blending" an ability the
//   player gets later on (and can decide if/when to use).
//   maybe make them control the individual exposure values (distance
//   to current position; distance to snap position) and make
//   this whole "scene with 100 layers and additive blending difficult to
//   understand, but only having a small visible radius around yourself
//   kinda sucks as well"-thing a game mechanic.
//   *why don't we make all value tweaking stuff literally just game mechanics?* :D
// - use additional visual clues to make understanding layered, blended
//   scenes easer? like also color them based on normal?
//   [tried in model.frag, kinda nice but not too helpful. can include
//    objects normals into that but probably not what we want]
// - repro pass alpha blending: do a depth pre pass and take objects
//   behind the first way less into account? poor-mans alpha blending
//   i guess...
// - this project should be awesome for temporal super sampling
//   i guess we could get away with depth-based rejection, right?
//   especially when we consider the scene static (i.e. no linearly
//   approximated movement visualization sensor module for now) we
//   should get pretty much ground truth with that? and it can be
//   useful for better snap (shadow map) sampling
//
// - getting more detailed snaps (shadow maps):
//   render high res and generate mipmaps? i guess the main performance
//   impact atm is sampling a super high-res shadow map in primitives
//   that are far away and outside of the snap.
// - huge performance improvement: frustum-culling the primitives in the
//   snap and only render them in the repro-depth passes afterwards (per frame).
//   wait, we can do even more than just frustum culling. We could even
//   perform occlusion culling in the snap. Maybe just use occlusion queries
//   for that?
//   Remember that the information we calculate there once brings huge
//   performance improvements in all following repro frames.
// - to not have all the high-res shadow map rendering have a huge impact
//   on playability (i.e. we want snap results fast! like in a few milliseconds)
//   start by rendering a low LOD and only when that is finished do all
//   the fancier stuff (async) like rendering the super high-res snap(s)

using namespace tkn::types;

class ViewApp : public tkn::App {
public:
	static constexpr auto snapFormat = vk::Format::d32Sfloat;
	static constexpr auto sceneFormat = vk::Format::r16Sfloat;
	static constexpr auto snapSize = vk::Extent2D {4096, 4096};
	static constexpr auto reproBlend = true;

	static constexpr float near = 0.01f;
	static constexpr float far = 10.f;
	static constexpr float fov = 0.5 * nytl::constants::pi;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		return true;
	}

	void initRenderData() override {
		// init pipeline
		auto& dev = vulkanDevice();
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
		sampler_ = {dev, sci};

		auto idata = std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u};
		auto span = nytl::as_bytes(nytl::span(idata));
		auto p = tkn::wrap({1u, 1u}, vk::Format::r8g8b8a8Unorm, span);
		tkn::TextureCreateParams params;
		params.format = vk::Format::r8g8b8a8Unorm;
		dummyTex_ = {batch, std::move(p), params};

		// Load scene
		auto s = sceneScale_;
		auto mat = tkn::scaleMat<4, float>(nytl::Vec3f{s, s, s});
		auto samplerAnisotropy = 1.f;
		if(anisotropy_) {
			samplerAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
		}
		auto ri = tkn::SceneRenderInfo{
			dummyTex_.vkImageView(),
			samplerAnisotropy, false, multiDrawIndirect_
		};
		auto [omodel, path] = tkn::loadGltf(modelname_);
		if(!omodel) {
			throw std::runtime_error("Failed to load model");
		}

		auto& model = *omodel;
		auto scene = model.defaultScene >= 0 ? model.defaultScene : 0;
		auto& sc = model.scenes[scene];

		auto initScene = vpp::InitObject<tkn::Scene>(scene_, batch, path,
			model, sc, mat, ri);
		initScene.init(batch, dummyTex_.vkImageView());

		// view + projection matrix
		auto sceneBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		sceneDsLayout_ = {dev, sceneBindings};

		// camera
		auto sceneUboSize = 2 * sizeof(nytl::Mat4f)
			+ sizeof(nytl::Vec3f) + sizeof(float) * 5;
		sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
		sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, hostMem};

		initSnap(cb);
		initRepro();
		initPP();

		// descriptors
		vpp::DescriptorSetUpdate sdsu(sceneDs_);
		sdsu.uniform({{{sceneUbo_}}});
		sdsu.apply();

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));
	}

	// Records the command buffer to render a snapshot
	void recordSnap(vk::CommandBuffer cb) {
		vk::beginCommandBuffer(cb, {});
		std::array<vk::ClearValue, 1u> cv {};
		cv[0].depthStencil = {1.f, 0u}; // depth
		vk::cmdBeginRenderPass(cb, {snap_.rp, snap_.fb,
			{0u, 0u, snapSize.width, snapSize.height},
			std::uint32_t(cv.size()), cv.data()}, {});

		vk::Viewport vp {0.f, 0.f, (float) snapSize.width, (float) snapSize.height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, snapSize.width, snapSize.height});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, snap_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, snap_.pipeLayout, 0, {sceneDs_});
		scene_.render(cb, snap_.pipeLayout, false); // opaque

		// NOTE: when taking snapthosts we actually don't care about
		// blending transparency at all. There are only solid objects
		// or air.
		scene_.render(cb, snap_.pipeLayout, true); // transparent/blend

		vk::cmdEndRenderPass(cb);
		vk::endCommandBuffer(cb);
	}

	void initSnap(vk::CommandBuffer cb) {
		auto& dev = vulkanDevice();

		// rp
		std::array<vk::AttachmentDescription, 1> attachments;
		attachments[0].initialLayout = vk::ImageLayout::undefined;
		attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[0].format = snapFormat;
		attachments[0].loadOp = vk::AttachmentLoadOp::clear;
		attachments[0].storeOp = vk::AttachmentStoreOp::store;
		attachments[0].samples = vk::SampleCountBits::e1;

		vk::AttachmentReference depthRef;
		depthRef.attachment = 0;
		depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.pDepthStencilAttachment = &depthRef;

		// make sure color and depth buffer are available afterwards
		vk::SubpassDependency dependency;
		dependency.srcSubpass = 0;
		dependency.srcStageMask =
			vk::PipelineStageBits::earlyFragmentTests |
			vk::PipelineStageBits::lateFragmentTests;
		dependency.srcAccessMask = vk::AccessBits::depthStencilAttachmentWrite;
		dependency.dstSubpass = vk::subpassExternal;
		dependency.dstStageMask = vk::PipelineStageBits::allGraphics;
		dependency.dstAccessMask = vk::AccessBits::shaderRead;

		vk::RenderPassCreateInfo rpi;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &subpass;
		rpi.attachmentCount = attachments.size();
		rpi.pAttachments = attachments.data();
		rpi.dependencyCount = 1u;
		rpi.pDependencies = &dependency;
		snap_.rp = {dev, rpi};

		// pipeline layout consisting of all ds layouts and pcrs
		snap_.pipeLayout = {dev, {{
			sceneDsLayout_.vkHandle(),
			scene_.dsLayout().vkHandle(),
		}}, {}};

		vpp::ShaderModule vertShader(dev, repro_model_vert_data);
		vpp::ShaderModule fragShader(dev, repro_snap_frag_data);
		vpp::GraphicsPipelineInfo gpi {snap_.rp, snap_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0};

		gpi.vertex = tkn::Scene::vertexInfo();
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		// no color attachment
		gpi.blend.attachmentCount = 0;
		gpi.blend.pAttachments = nullptr;

		// NOTE: see the gltf material.doubleSided property. We can't switch
		// this per material (without requiring two pipelines) so we simply
		// always render backfaces currently and then dynamically cull in the
		// fragment shader. That is required since some models rely on
		// backface culling for effects (e.g. outlines). See model.frag
		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		gpi.rasterization.depthBiasEnable = true;
		gpi.rasterization.depthBiasConstantFactor = 2.f;
		gpi.rasterization.depthBiasSlopeFactor = 5.f;

		snap_.pipe = {dev, gpi.info()};

		snap_.semaphore = {dev};
		auto qf = dev.queueSubmitter().queue().family();
		snap_.cb = dev.commandAllocator().get(qf,
			vk::CommandPoolCreateBits::resetCommandBuffer);

		// image and fb
		auto info = vpp::ViewableImageCreateInfo(snapFormat,
			vk::ImageAspectBits::depth, snapSize,
			vk::ImageUsageBits::sampled |
				vk::ImageUsageBits::depthStencilAttachment |
				vk::ImageUsageBits::transferDst);
		dlg_assert(vpp::supported(dev, info.img));
		snap_.depth = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		// initial transition of snap depth image to read only
		{
			vk::ImageMemoryBarrier obarrier;
			obarrier.image = snap_.depth.image();
			obarrier.oldLayout = vk::ImageLayout::undefined;
			obarrier.newLayout = vk::ImageLayout::transferDstOptimal;
			obarrier.dstAccessMask = vk::AccessBits::transferWrite;
			obarrier.subresourceRange = {vk::ImageAspectBits::depth, 0, 1, 0, 1};
			vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
				vk::PipelineStageBits::transfer, {}, {}, {}, {{obarrier}});

			auto depth = vk::ClearDepthStencilValue {1.f, 0u};
			vk::cmdClearDepthStencilImage(cb, snap_.depth.vkImage(),
				vk::ImageLayout::transferDstOptimal, depth,
				{{{vk::ImageAspectBits::depth, 0, 1, 0, 1}}});

			obarrier.oldLayout = vk::ImageLayout::transferDstOptimal;
			obarrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
			obarrier.srcAccessMask = vk::AccessBits::transferWrite;
			obarrier.dstAccessMask = vk::AccessBits::shaderRead;
			obarrier.subresourceRange = {vk::ImageAspectBits::depth, 0, 1, 0, 1};
			vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
				vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{obarrier}});
		}

		auto fbatts = {
			snap_.depth.vkImageView(),
		};
		auto fbi = vk::FramebufferCreateInfo({}, snap_.rp,
			fbatts.size(), fbatts.begin(),
			snapSize.width, snapSize.height, 1);
		snap_.fb = {dev, fbi};
	}

	void initRepro() {
		auto& dev = vulkanDevice();

		// rp
		std::array<vk::AttachmentDescription, 2> attachments;
		attachments[0].initialLayout = vk::ImageLayout::undefined;
		attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[0].format = sceneFormat;
		attachments[0].loadOp = vk::AttachmentLoadOp::clear;
		attachments[0].storeOp = vk::AttachmentStoreOp::store;
		attachments[0].samples = vk::SampleCountBits::e1;

		vk::AttachmentReference colorRef, depthRef;
		colorRef.attachment = 0;
		colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;

		if(!reproBlend) {
			depthRef.attachment = 1;
			depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

			attachments[1].initialLayout = vk::ImageLayout::undefined;
			attachments[1].finalLayout = vk::ImageLayout::depthStencilAttachmentOptimal;
			attachments[1].format = depthFormat();
			attachments[1].loadOp = vk::AttachmentLoadOp::clear;
			attachments[1].storeOp = vk::AttachmentStoreOp::dontCare;
			attachments[1].samples = vk::SampleCountBits::e1;

			subpass.pDepthStencilAttachment = &depthRef;
		}

		// make sure depth buffer is available afterwards
		vk::SubpassDependency dependency;
		dependency.srcSubpass = 0;
		dependency.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dependency.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		dependency.dstSubpass = vk::subpassExternal;
		dependency.dstStageMask = vk::PipelineStageBits::fragmentShader;
		dependency.dstAccessMask = vk::AccessBits::shaderRead;

		vk::RenderPassCreateInfo rpi;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &subpass;
		rpi.attachmentCount = reproBlend ? 1 : 2;
		rpi.pAttachments = attachments.data();
		rpi.dependencyCount = 1u;
		rpi.pDependencies = &dependency;
		repro_.rp = {dev, rpi};

		// layouts
		auto snapBindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		repro_.snapDsLayout = {dev, snapBindings};
		repro_.snapDs = {dev.descriptorAllocator(), repro_.snapDsLayout};
		repro_.pipeLayout = {dev, {{
			sceneDsLayout_.vkHandle(),
			scene_.dsLayout().vkHandle(),
			repro_.snapDsLayout.vkHandle(),
		}}, {}};

		vpp::DescriptorSetUpdate dsu(repro_.snapDs);
		dsu.imageSampler({{{}, snap_.depth.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.apply();

		// pipe
		vpp::ShaderModule vertShader(dev, repro_model_vert_data);
		vpp::ShaderModule fragShader(dev, repro_model_frag_data);
		vpp::GraphicsPipelineInfo gpi {repro_.rp, repro_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}};

		gpi.vertex = tkn::Scene::vertexInfo();
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		if(reproBlend) {
			// purely additive blending
			static const vk::PipelineColorBlendAttachmentState blendAttachment = {
				true,

				vk::BlendFactor::one,
				vk::BlendFactor::one,
				vk::BlendOp::add,

				vk::BlendFactor::one,
				vk::BlendFactor::one,
				vk::BlendOp::add,

				vk::ColorComponentBits::r |
					vk::ColorComponentBits::g |
					vk::ColorComponentBits::b |
					vk::ColorComponentBits::a,
			};

			gpi.blend.attachmentCount = 1;
			gpi.blend.pAttachments = &blendAttachment;

			gpi.depthStencil.depthTestEnable = false;
			gpi.depthStencil.depthWriteEnable = false;
		} else {
			gpi.depthStencil.depthTestEnable = true;
			gpi.depthStencil.depthWriteEnable = true;
			gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

			gpi.blend.attachmentCount = 1;
			gpi.blend.pAttachments = &tkn::noBlendAttachment();
		}

		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		repro_.pipe = {dev, gpi.info()};
	}

	void initPP() {
		auto& dev = vulkanDevice();

		// layouts
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		pp_.dsLayout = {dev, bindings};
		pp_.pipeLayout = {dev, {{pp_.dsLayout}}, {}};

		vpp::ShaderModule vertShader(dev, tkn_fullscreen_vert_data);
		vpp::ShaderModule fragShader(dev, repro_pp_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderPass(), pp_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0};

		auto atts = {tkn::noBlendAttachment()};
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.attachmentCount = atts.size();
		gpi.blend.pAttachments = atts.begin();
		pp_.pipe = {dev, gpi.info()};

		// ubo
		auto s = sizeof(float);
		pp_.ubo = {dev.bufferAllocator(), s, vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes()};

		pp_.ds = {dev.descriptorAllocator(), pp_.dsLayout};
	}

	bool features(tkn::Features& enable, const tkn::Features& supported) override {
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

	void initBuffers(const vk::Extent2D& size,
			nytl::Span<RenderBuffer> bufs) override {
		App::initBuffers(size, bufs);
		auto& dev = vulkanDevice();

		auto info = vpp::ViewableImageCreateInfo(sceneFormat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::sampled |
				vk::ImageUsageBits::colorAttachment);
		dlg_assert(vpp::supported(dev, info.img));
		repro_.image = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		if(!reproBlend) {
			repro_.depth = createDepthTarget(size);
		}

		auto attachments = {repro_.image.vkImageView(), repro_.depth.vkImageView()};
		auto fbi = vk::FramebufferCreateInfo({}, repro_.rp,
			2 - int(reproBlend), attachments.begin(),
			size.width, size.height, 1);
		repro_.fb = {dev, fbi};

		// update ds
		vpp::DescriptorSetUpdate dsu(pp_.ds);
		dsu.imageSampler({{{}, repro_.image.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.uniform({{{pp_.ubo}}});
		dsu.apply();

		recordSnap(snap_.cb);
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);

		// depth/repro pass
		auto [width, height] = swapchainInfo().imageExtent;
		vk::ClearValue cvs[2] {};
		cvs[0].color = {0.1f, 0.1f, 0.1f, 0.f};
		cvs[1].depthStencil = {1.f, 0u};
		vk::cmdBeginRenderPass(cb, {repro_.rp, repro_.fb,
			{0u, 0u, width, height}, 2 - reproBlend, cvs}, {});

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, repro_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, repro_.pipeLayout, 0, {sceneDs_});
		tkn::cmdBindGraphicsDescriptors(cb, repro_.pipeLayout, 2, {repro_.snapDs});
		// NOTE: again, we treat opaque and blended objects equally
		scene_.render(cb, repro_.pipeLayout, false);
		scene_.render(cb, repro_.pipeLayout, true);
		vk::cmdEndRenderPass(cb);
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, pp_.pipeLayout, 0, {pp_.ds});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad
	}

	void update(double dt) override {
		App::update(dt);
		time_ += dt;

		// movement
		auto kc = appContext().keyboardContext();
		if(kc) {
			tkn::checkMovement(camera_, *kc, dt);
		}

		if(snap_.pending) {
			snap_.time = time_;
			snap_.vp = cameraVP();
			camera_.update = true; // trigger scene ubo update
			snap_.pending = false;
		}

		// we currently always redraw to see consistent fps
		App::scheduleRedraw();
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		switch(ev.keycode) {
			case ny::Keycode::up:
				pp_.exposure *= 1.1;
				pp_.update = true;
				dlg_info("exposure: {}", pp_.exposure);
				return true;
			case ny::Keycode::down:
				pp_.exposure /= 1.1;
				pp_.update = true;
				dlg_info("exposure: {}", pp_.exposure);
				return true;
			case ny::Keycode::left:
				repro_.exposure *= 1.1;
				dlg_info("repro exposure: {}", repro_.exposure);
				return true;
			case ny::Keycode::right:
				repro_.exposure /= 1.1;
				dlg_info("repro exposure: {}", repro_.exposure);
				return true;
			case ny::Keycode::space: {
				auto& qs = vulkanDevice().queueSubmitter();
				vk::SubmitInfo si;
				si.commandBufferCount = 1;
				si.pCommandBuffers = &snap_.cb.vkHandle();
				si.signalSemaphoreCount = 1;
				si.pSignalSemaphores = &snap_.semaphore.vkHandle();
				qs.add(si);
				snap_.pending = true;
				App::addSemaphore(snap_.semaphore,
					vk::PipelineStageBits::allGraphics);
				return true;
			} default:
				break;
		}

		return false;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			tkn::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
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

	nytl::Mat4f projectionMatrix() const {
		auto aspect = float(window().size().x) / window().size().y;
		return tkn::perspective(fov, aspect, -near, -far);
	}

	nytl::Mat4f cameraVP() const {
		return projectionMatrix() * viewMatrix(camera_);
	}

	void updateDevice() override {
		// update scene ubo every frame (for time)
		{
			auto map = sceneUbo_.memoryMap();
			auto span = map.span();
			tkn::write(span, cameraVP());
			tkn::write(span, camera_.pos);
			tkn::write(span, time_);
			tkn::write(span, snap_.vp);
			tkn::write(span, snap_.time);
			tkn::write(span, near);
			tkn::write(span, far);
			tkn::write(span, repro_.exposure);
			map.flush();
		}

		if(pp_.update) {
			auto map = pp_.ubo.memoryMap();
			auto span = map.span();
			tkn::write(span, pp_.exposure);
			map.flush();
		}

		auto semaphore = scene_.updateDevice(cameraVP());
		if(semaphore) {
			addSemaphore(semaphore, vk::PipelineStageBits::allGraphics);
			// HACK: we need to trigger initBuffers to rerecord all cb's...
			// App::scheduleRerecord();
			App::resize_ = true;
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.update = true;
	}

	const char* name() const override { return "repro"; }

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

	vpp::TrDsLayout sceneDsLayout_;
	vpp::SubBuffer sceneUbo_;
	vpp::TrDs sceneDs_;

	struct {
		vpp::RenderPass rp;
		vpp::CommandBuffer cb;
		vpp::ViewableImage depth;
		vpp::Framebuffer fb;
		vpp::Semaphore semaphore;
		vpp::Pipeline pipe;
		vpp::PipelineLayout pipeLayout;
		float time {0.f};
		nytl::Mat4f vp;
		bool pending {false};
	} snap_;

	// reprojection depth pass; actually rendering the scene
	// in every frame
	struct {
		vpp::ViewableImage image;
		vpp::ViewableImage depth;
		vpp::Pipeline pipe;
		vpp::Framebuffer fb;
		vpp::RenderPass rp;
		vpp::PipelineLayout pipeLayout;

		vpp::TrDsLayout snapDsLayout;
		vpp::TrDs snapDs;
		float exposure {1.f};
	} repro_;

	// post processing pass
	struct {
		vpp::Pipeline pipe;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::SubBuffer ubo;
		bool update {true};
		float exposure {1.f};
	} pp_;

	tkn::Texture dummyTex_;

	bool anisotropy_ {}; // whether device supports anisotropy
	bool multiview_ {}; // whether device supports multiview
	bool depthClamp_ {}; // whether the device supports depth clamping
	bool multiDrawIndirect_ {};

	float time_ {};
	bool rotateView_ {false}; // mouseLeft down

	tkn::Scene scene_; // no default constructor
	tkn::Camera camera_ {};

	// args
	std::string modelname_ {};
	float sceneScale_ {1.f};
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
