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
#include <tkn/scene/light.hpp>
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
#include <shaders/repro.depth.frag.h>
#include <shaders/repro.repro.frag.h>

#include <optional>
#include <vector>
#include <string>

// TODO: currently contains lots of potentially unneeded stuff (lights etc)
// from br. Can be cleaned up

// Basically three render passes:
// - snap: only executed every time a new snapshot is taken. Renders into
//   the snap image that is read from
// - depth only: rendered every frame, only renders depth of whole scene
// - repro: final pass of every frame. Takes rendered depth and the last
//   snap and reprojects the snap onto the depth

using namespace tkn::types;

class ViewApp : public tkn::App {
public:
	static constexpr auto snapFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto snapSize = vk::Extent2D {2 * 4096, 2 * 4096};

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		camera_.perspective.near = 0.01f;
		camera_.perspective.far = 10.f;

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

		vpp::Init<tkn::Texture> initBrdfLut(batch, tkn::read("brdflut.ktx"));
		brdfLut_ = initBrdfLut.init(batch);

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
		auto mat = tkn::scaleMat<4, float>({s, s, s});
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

		shadowData_ = tkn::initShadowData(dev, depthFormat(),
			scene_.dsLayout(), multiview_, depthClamp_);

		// view + projection matrix
		auto cameraBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		cameraDsLayout_ = {dev, cameraBindings};

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

		// box indices
		std::array<std::uint16_t, 36> indices = {
			0, 1, 2,  2, 1, 3, // front
			1, 5, 3,  3, 5, 7, // right
			2, 3, 6,  6, 3, 7, // top
			4, 0, 6,  6, 0, 2, // left
			4, 5, 0,  0, 5, 1, // bottom
			5, 4, 7,  7, 4, 6, // back
		};

		boxIndices_ = {alloc.bufDevice,
			36u * sizeof(std::uint16_t),
			vk::BufferUsageBits::indexBuffer | vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes(), 4u};
		auto boxIndicesStage = vpp::fillStaging(cb, boxIndices_, indices);

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

		initSnap();
		initDepth();
		initRepr();

		tkn::Environment::InitData initEnv;
		env_.create(initEnv, batch, "convolution.ktx", "irradiance.ktx", sampler_);
		env_.createPipe(device(), cameraDsLayout_, snap_.rp, 0u, samples());
		env_.init(initEnv, batch);

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
	}

	void recordSnap(vk::CommandBuffer cb) {
		vk::beginCommandBuffer(cb, {});
		std::array<vk::ClearValue, 2u> cv {};
		cv[0] = {{0.f, 0.f, 0.f, 0.f}}; // color
		cv[1].depthStencil = {1.f, 0u}; // depth
		vk::cmdBeginRenderPass(cb, {snap_.rp, snap_.fb,
			{0u, 0u, snapSize.width, snapSize.height},
			std::uint32_t(cv.size()), cv.data()}, {});

		vk::Viewport vp{0.f, 0.f, (float) snapSize.width, (float) snapSize.height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, snapSize.width, snapSize.height});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, snap_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, snap_.pipeLayout, 0, {cameraDs_});
		tkn::cmdBindGraphicsDescriptors(cb, snap_.pipeLayout, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, snap_.pipeLayout, false); // opaque

		vk::cmdBindIndexBuffer(cb, boxIndices_.buffer(),
			boxIndices_.offset(), vk::IndexType::uint16);
		tkn::cmdBindGraphicsDescriptors(cb, env_.pipeLayout(), 0,
			{envCameraDs_});
		env_.render(cb);

		// vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, blendPipe_);
		// tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		// tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
		// 	dirLight_.ds(), pointLight_.ds(), aoDs_});
		// scene_.render(cb, pipeLayout_, true); // transparent/blend

		vk::cmdEndRenderPass(cb);
		vk::endCommandBuffer(cb);

	}

	void initSnap() {
		auto& dev = vulkanDevice();

		// rp
		std::array<vk::AttachmentDescription, 2> attachments;
		attachments[0].initialLayout = vk::ImageLayout::undefined;
		attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[0].format = snapFormat;
		attachments[0].loadOp = vk::AttachmentLoadOp::clear;
		attachments[0].storeOp = vk::AttachmentStoreOp::store;
		attachments[0].samples = vk::SampleCountBits::e1;

		attachments[1].initialLayout = vk::ImageLayout::undefined;
		attachments[1].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[1].format = depthFormat();
		attachments[1].loadOp = vk::AttachmentLoadOp::clear;
		attachments[1].storeOp = vk::AttachmentStoreOp::store;
		attachments[1].samples = vk::SampleCountBits::e1;

		vk::AttachmentReference colorRefs[1];
		colorRefs[0].attachment = 0;
		colorRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;

		vk::AttachmentReference depthRef;
		depthRef.attachment = 1;
		depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = colorRefs;
		subpass.pDepthStencilAttachment = &depthRef;

		// make sure color and depth buffer are available afterwards
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
			cameraDsLayout_.vkHandle(),
			scene_.dsLayout().vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			aoDsLayout_.vkHandle(),
		}}, {}};

		vpp::ShaderModule vertShader(dev, repro_model_vert_data);
		vpp::ShaderModule fragShader(dev, repro_model_frag_data);
		vpp::GraphicsPipelineInfo gpi {snap_.rp, snap_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0};

		gpi.vertex = tkn::Scene::vertexInfo();
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		// no blending at all, alpha contains linear depth (z)
		auto atts = {tkn::noBlendAttachment()};
		gpi.blend.attachmentCount = atts.size();
		gpi.blend.pAttachments = atts.begin();

		// NOTE: see the gltf material.doubleSided property. We can't switch
		// this per material (without requiring two pipelines) so we simply
		// always render backfaces currently and then dynamically cull in the
		// fragment shader. That is required since some models rely on
		// backface culling for effects (e.g. outlines). See model.frag
		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		gpi.rasterization.depthBiasEnable = true;
		gpi.rasterization.depthBiasConstantFactor = 5.f;
		gpi.rasterization.depthBiasSlopeFactor = 15.f;

		snap_.pipe = {dev, gpi.info()};

		// gpi.depthStencil.depthWriteEnable = false;
		// snap_.blendPipe = {dev, gpi.info()};

		snap_.semaphore = {dev};
		auto qf = dev.queueSubmitter().queue().family();
		snap_.cb = dev.commandAllocator().get(qf,
			vk::CommandPoolCreateBits::resetCommandBuffer);

		// image and fb
		auto info = vpp::ViewableImageCreateInfo(snapFormat,
			vk::ImageAspectBits::color, snapSize,
			vk::ImageUsageBits::sampled |
				vk::ImageUsageBits::colorAttachment |
				vk::ImageUsageBits::transferDst);
		dlg_assert(vpp::supported(dev, info.img));
		snap_.image = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
		snap_.depth = createDepthTarget(snapSize);

		// initial transition of snap image to read only
		{
			auto& qs = dev.queueSubmitter();
			auto qfam = qs.queue().family();
			auto cb = dev.commandAllocator().get(qfam);
			vk::beginCommandBuffer(cb, {});

			vk::ImageMemoryBarrier obarrier;
			obarrier.image = snap_.image.image();
			obarrier.oldLayout = vk::ImageLayout::undefined;
			obarrier.newLayout = vk::ImageLayout::transferDstOptimal;
			obarrier.dstAccessMask = vk::AccessBits::transferWrite;
			obarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
			vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
				vk::PipelineStageBits::transfer, {}, {}, {}, {{obarrier}});

			auto color = vk::ClearColorValue {{0.f, 0.f, 0.f, 0.f}};
			vk::cmdClearColorImage(cb, snap_.image.vkImage(),
				vk::ImageLayout::transferDstOptimal, color,
				{{{vk::ImageAspectBits::color, 0, 1, 0, 1}}});

			obarrier.oldLayout = vk::ImageLayout::transferDstOptimal;
			obarrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
			obarrier.srcAccessMask = vk::AccessBits::transferWrite;
			obarrier.dstAccessMask = vk::AccessBits::shaderRead;
			obarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
			vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
				vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{obarrier}});

			// TODO: don't block...
			vk::endCommandBuffer(cb);
			qs.wait(qs.add(cb));
		}

		auto fbatts = {
			snap_.image.vkImageView(),
			snap_.depth.vkImageView(),
		};
		auto fbi = vk::FramebufferCreateInfo({}, snap_.rp,
			fbatts.size(), fbatts.begin(),
			snapSize.width, snapSize.height, 1);
		snap_.fb = {dev, fbi};
	}

	void initDepth() {
		auto& dev = vulkanDevice();

		// rp
		std::array<vk::AttachmentDescription, 1> attachments;
		attachments[0].initialLayout = vk::ImageLayout::undefined;
		attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[0].format = depthFormat();
		attachments[0].loadOp = vk::AttachmentLoadOp::clear;
		attachments[0].storeOp = vk::AttachmentStoreOp::store;
		attachments[0].samples = vk::SampleCountBits::e1;

		vk::AttachmentReference depthRef;
		depthRef.attachment = 0;
		depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.pDepthStencilAttachment = &depthRef;

		// make sure depth buffer is available afterwards
		vk::SubpassDependency dependency;
		dependency.srcSubpass = 0;
		dependency.srcStageMask =
			vk::PipelineStageBits::earlyFragmentTests |
			vk::PipelineStageBits::lateFragmentTests;
		dependency.srcAccessMask =
			vk::AccessBits::depthStencilAttachmentWrite;
		dependency.dstSubpass = vk::subpassExternal;
		dependency.dstStageMask = vk::PipelineStageBits::fragmentShader;
		dependency.dstAccessMask = vk::AccessBits::shaderRead;

		vk::RenderPassCreateInfo rpi;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &subpass;
		rpi.attachmentCount = attachments.size();
		rpi.pAttachments = attachments.data();
		rpi.dependencyCount = 1u;
		rpi.pDependencies = &dependency;
		depth_.rp = {dev, rpi};

		// pipe
		vpp::ShaderModule vertShader(dev, repro_model_vert_data);
		vpp::ShaderModule fragShader(dev, repro_depth_frag_data);
		vpp::GraphicsPipelineInfo gpi {depth_.rp, snap_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}};

		gpi.vertex = tkn::Scene::vertexInfo();
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		gpi.blend.attachmentCount = 0;
		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		depth_.pipe = {dev, gpi.info()};
	}

	void initRepr() {
		auto& dev = vulkanDevice();

		// layouts
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		repr_.dsLayout = {dev, bindings};
		repr_.pipeLayout = {dev, {{repr_.dsLayout}}, {}};

		vpp::ShaderModule vertShader(dev, tkn_fullscreen_vert_data);
		vpp::ShaderModule fragShader(dev, repro_repro_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderPass(), repr_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0};

		auto atts = {tkn::noBlendAttachment()};
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.attachmentCount = atts.size();
		gpi.blend.pAttachments = atts.begin();
		repr_.pipe = {dev, gpi.info()};

		// ubo
		auto s = sizeof(nytl::Mat4f) * 2 + sizeof(nytl::Vec3f) + sizeof(float) * 5;
		repr_.ubo = {dev.bufferAllocator(), s, vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes()};

		repr_.ds = {dev.descriptorAllocator(), repr_.dsLayout};
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

		depth_.image = createDepthTarget(size);
		auto fbi = vk::FramebufferCreateInfo({}, depth_.rp,
			1u, &depth_.image.vkImageView(),
			size.width, size.height, 1);
		depth_.fb = {dev, fbi};

		// update ds
		vpp::DescriptorSetUpdate dsu(repr_.ds);
		dsu.uniform({{{repr_.ubo}}});
		// dsu.imageSampler({{{}, snap_.image.vkImageView(),
		// 	vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, snap_.depth.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, depth_.image.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.apply();

		recordSnap(snap_.cb);
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);
		if(mode_ & modeDirLight) {
			dirLight_.render(cb, shadowData_, scene_);
		}
		if(mode_ & modePointLight) {
			pointLight_.render(cb, shadowData_, scene_);
		}

		// depth pass
		auto [width, height] = swapchainInfo().imageExtent;
		vk::ClearValue cv {};
		cv.depthStencil = {1.f, 0u};
		vk::cmdBeginRenderPass(cb, {depth_.rp, depth_.fb,
			{0u, 0u, width, height}, 1, &cv}, {});

		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, depth_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, snap_.pipeLayout, 0, {cameraDs_});
		tkn::cmdBindGraphicsDescriptors(cb, snap_.pipeLayout, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, snap_.pipeLayout, false); // opaque
		vk::cmdEndRenderPass(cb);
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, repr_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, repr_.pipeLayout, 0, {repr_.ds});
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

		if(snap_.pending) {
			snap_.time = time_;
			snap_.vp = matrix(camera_);
			snap_.pending = false;
			repr_.update = true;
		}

		// we currently always redraw to see consistent fps
		// if(moveLight_ || camera_.update || updateLight_ || updateAOParams_) {
		// 	App::scheduleRedraw();
		// }

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
			case ny::Keycode::m: // move light here
				moveLight_ = false;
				dirLight_.data.dir = -camera_.pos;
				scene_.instances()[dirLight_.instanceID].matrix = dirLightObjMatrix();
				scene_.updatedInstance(dirLight_.instanceID);
				updateLight_ = true;
				return true;
			case ny::Keycode::n: // move light here
				moveLight_ = false;
				updateLight_ = true;
				pointLight_.data.position = camera_.pos;
				scene_.instances()[pointLight_.instanceID].matrix = pointLightObjMatrix();
				scene_.updatedInstance(pointLight_.instanceID);
				return true;
			case ny::Keycode::l:
				moveLight_ ^= true;
				return true;
			case ny::Keycode::p:
				dirLight_.data.flags ^= tkn::lightFlagPcf;
				pointLight_.data.flags ^= tkn::lightFlagPcf;
				updateLight_ = true;
				return true;
			case ny::Keycode::up:
				repr_.exposure *= 1.1;
				dlg_info("exposure: {}", repr_.exposure);
				return true;
			case ny::Keycode::down:
				repr_.exposure /= 1.1;
				dlg_info("exposure: {}", repr_.exposure);
				return true;
			case ny::Keycode::k1:
				mode_ ^= modeDirLight;
				updateLight_ = true;
				updateAOParams_ = true;
				App::scheduleRerecord();
				dlg_info("dir light: {}", bool(mode_ & modeDirLight));
				return true;
			case ny::Keycode::k2:
				mode_ ^= modePointLight;
				updateLight_ = true;
				updateAOParams_ = true;
				App::scheduleRerecord();
				dlg_info("point light: {}", bool(mode_ & modePointLight));
				return true;
			case ny::Keycode::k3:
				mode_ ^= modeSpecularIBL;
				dlg_info("specular IBL: {}", bool(mode_ & modeSpecularIBL));
				updateAOParams_ = true;
				return true;
			case ny::Keycode::k4:
				mode_ ^= modeIrradiance;
				updateAOParams_ = true;
				dlg_info("Static AO: {}", bool(mode_ & modeIrradiance));
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

	void updateDevice() override {
		// update scene ubo
		if(camera_.update) {
			camera_.update = false;
			updateLight_ = true;
			repr_.update = true;


			{
				auto map = cameraUbo_.memoryMap();
				auto span = map.span();
				tkn::write(span, matrix(camera_));
				tkn::write(span, camera_.pos);
				tkn::write(span, camera_.perspective.near);
				tkn::write(span, camera_.perspective.far);
				map.flush();
			}

			{
				auto envMap = envCameraUbo_.memoryMap();
				auto envSpan = envMap.span();
				tkn::write(envSpan, fixedMatrix(camera_));
				envMap.flush();
			}
		}

		// always update e.g. for time_
		if(/*repr_.update*/ true) {
			repr_.update = false;
			auto map = repr_.ubo.memoryMap();
			auto span = map.span();
			tkn::write(span, snap_.vp);
			tkn::write(span, nytl::Mat4f(nytl::inverse(matrix(camera_))));
			tkn::write(span, camera_.pos);
			tkn::write(span, camera_.perspective.near);
			tkn::write(span, camera_.perspective.far);
			tkn::write(span, snap_.time);
			tkn::write(span, time_);
			tkn::write(span, repr_.exposure);
			map.flush();
		}

		auto semaphore = scene_.updateDevice(matrix(camera_));
		if(semaphore) {
			addSemaphore(semaphore, vk::PipelineStageBits::allGraphics);
			// HACK: trigger initBuffers to rerecord all cb's...
			// App::scheduleRerecord();
			App::resize_ = true;
		}

		if(updateLight_) {
			if(mode_ & modeDirLight) {
				dirLight_.updateDevice(camera_);
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

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
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
	vpp::TrDsLayout cameraDsLayout_;

	vpp::SubBuffer cameraUbo_;
	vpp::TrDs cameraDs_;
	vpp::SubBuffer envCameraUbo_;
	vpp::TrDs envCameraDs_;

	vpp::TrDsLayout aoDsLayout_;
	vpp::TrDs aoDs_;
	vpp::SubBuffer aoUbo_;
	bool updateAOParams_ {true};
	float aoFac_ {0.1f};

	struct {
		vpp::ViewableImage image;
		vpp::Pipeline pipe;
		vpp::Framebuffer fb;
		vpp::RenderPass rp;
	} depth_;

	struct {
		vpp::RenderPass rp;
		vpp::CommandBuffer cb;
		vpp::ViewableImage depth;
		vpp::ViewableImage image;
		vpp::Framebuffer fb;
		vpp::Semaphore semaphore;
		vpp::Pipeline pipe;
		vpp::PipelineLayout pipeLayout;
		float time {0.f};
		nytl::Mat4f vp;
		bool pending {false};
	} snap_;

	struct {
		vpp::Pipeline pipe;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::SubBuffer ubo;
		bool update {true};
		float exposure {1.f};
	} repr_;

	static constexpr u32 modeDirLight = (1u << 0);
	static constexpr u32 modePointLight = (1u << 1);
	static constexpr u32 modeSpecularIBL = (1u << 2);
	static constexpr u32 modeIrradiance = (1u << 3);
	u32 mode_ {modePointLight | modeIrradiance | modeSpecularIBL};

	tkn::Texture dummyTex_;
	bool moveLight_ {false};

	bool anisotropy_ {}; // whether device supports anisotropy
	bool multiview_ {}; // whether device supports multiview
	bool depthClamp_ {}; // whether the device supports depth clamping
	bool multiDrawIndirect_ {};

	float time_ {};
	bool rotateView_ {false}; // mouseLeft down

	tkn::Scene scene_; // no default constructor
	tkn::Camera camera_ {};

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
	std::string modelname_ {};
	float sceneScale_ {1.f};

	vpp::SubBuffer boxIndices_;
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
