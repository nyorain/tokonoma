// WIP: playing around, might be in an ugly state
// Based upon br

#include <stage/camera.hpp>
#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/gltf.hpp>
#include <stage/quaternion.hpp>
#include <stage/scene/shape.hpp>
#include <stage/scene/scene.hpp>
#include <stage/scene/light.hpp>
#include <stage/scene/scene.hpp>
#include <stage/scene/environment.hpp>
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

#include <shaders/stage.fullscreen.vert.h>
#include <shaders/br.model.vert.h>
#include <shaders/br.model.frag.h>
#include <shaders/taa.pp.frag.h>
#include <shaders/taa.taa.comp.h>

#include <optional>
#include <vector>
#include <string>

using namespace doi::types;

class ViewApp : public doi::App {
public:
	static constexpr auto historyFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto offscreenFormat = vk::Format::r16g16b16a16Sfloat;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!doi::App::init(args)) {
			return false;
		}

		camera_.perspective.near = 0.05f;
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
		doi::WorkBatcher batch{dev, cb, {
				alloc.memDevice, alloc.memHost, memStage,
				alloc.bufDevice, alloc.bufHost, bufStage,
				dev.descriptorAllocator(),
			}
		};

		vpp::Init<doi::Texture> initBrdfLut(batch, doi::read("brdflut.ktx"));
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
		linearSampler_ = {dev, sci};

		sci.magFilter = vk::Filter::nearest;
		sci.minFilter = vk::Filter::nearest;
		sci.mipmapMode = vk::SamplerMipmapMode::nearest;
		nearestSampler_ = {dev, sci};

		auto idata = std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u};
		auto span = nytl::as_bytes(nytl::span(idata));
		auto p = doi::wrap({1u, 1u}, vk::Format::r8g8b8a8Unorm, span);
		doi::TextureCreateParams params;
		params.format = vk::Format::r8g8b8a8Unorm;
		dummyTex_ = {batch, std::move(p), params};

		// Load scene
		auto s = sceneScale_;
		auto mat = doi::scaleMat<4, float>({s, s, s});
		auto samplerAnisotropy = 1.f;
		if(anisotropy_) {
			samplerAnisotropy = dev.properties().limits.maxSamplerAnisotropy;
		}
		auto ri = doi::SceneRenderInfo{
			dummyTex_.vkImageView(),
			samplerAnisotropy, false, multiDrawIndirect_
		};
		auto [omodel, path] = doi::loadGltf(modelname_);
		if(!omodel) {
			throw std::runtime_error("Couldn't load gltf file");
		}

		auto& model = *omodel;
		auto scene = model.defaultScene >= 0 ? model.defaultScene : 0;
		auto& sc = model.scenes[scene];

		auto initScene = vpp::InitObject<doi::Scene>(scene_, batch, path,
			model, sc, mat, ri);
		initScene.init(batch, dummyTex_.vkImageView());

		shadowData_ = doi::initShadowData(dev, depthFormat(),
			scene_.dsLayout(), multiview_, depthClamp_);

		// view + projection matrix
		auto cameraBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		cameraDsLayout_ = {dev, cameraBindings};

		// ao
		auto sampler = &linearSampler_.vkHandle();
		auto aoBindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, sampler),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, sampler),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, sampler),
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		aoDsLayout_ = {dev, aoBindings};

		// pipeline layout consisting of all ds layouts and pcrs
		pipeLayout_ = {dev, {{
			cameraDsLayout_.vkHandle(),
			scene_.dsLayout().vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			aoDsLayout_.vkHandle(),
		}}, {}};

		// offscreen render pass
		std::array<vk::AttachmentDescription, 2> attachments;
		attachments[0].initialLayout = vk::ImageLayout::undefined;
		attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[0].format = offscreenFormat;
		attachments[0].loadOp = vk::AttachmentLoadOp::clear;
		attachments[0].storeOp = vk::AttachmentStoreOp::store;
		attachments[0].samples = vk::SampleCountBits::e1;

		attachments[1].initialLayout = vk::ImageLayout::undefined;
		attachments[1].finalLayout = vk::ImageLayout::depthStencilReadOnlyOptimal;
		attachments[1].format = depthFormat();
		attachments[1].loadOp = vk::AttachmentLoadOp::clear;
		attachments[1].storeOp = vk::AttachmentStoreOp::store;
		attachments[1].samples = vk::SampleCountBits::e1;

		vk::AttachmentReference colorRef;
		colorRef.attachment = 0;
		colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

		vk::AttachmentReference depthRef;
		depthRef.attachment = 1;
		depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;
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
		dependency.dstStageMask = vk::PipelineStageBits::computeShader;
		dependency.dstAccessMask = vk::AccessBits::shaderRead;

		vk::RenderPassCreateInfo rpi;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &subpass;
		rpi.attachmentCount = attachments.size();
		rpi.pAttachments = attachments.data();
		rpi.dependencyCount = 1u;
		rpi.pDependencies = &dependency;
		offscreen_.rp = {dev, rpi};

		// pipeline
		vpp::ShaderModule vertShader(dev, br_model_vert_data);
		vpp::ShaderModule fragShader(dev, br_model_frag_data);
		vpp::GraphicsPipelineInfo gpi {offscreen_.rp, pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}};

		gpi.vertex = doi::Scene::vertexInfo();
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
		pipe_ = {dev, gpi.info()};

		// gpi.depthStencil.depthWriteEnable = false;
		blendPipe_ = {dev, gpi.info()};

		doi::Environment::InitData initEnv;
		env_.create(initEnv, batch, "convolution.ktx", "irradiance.ktx", linearSampler_);
		env_.createPipe(device(), cameraDsLayout_, offscreen_.rp, 0u, samples());
		env_.init(initEnv, batch);

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
		auto cube = doi::Cube{{}, {0.05f, 0.05f, 0.05f}};
		auto shape = doi::generate(cube);
		cubePrimitiveID_ = scene_.addPrimitive(std::move(shape.positions),
			std::move(shape.normals), std::move(shape.indices));

		auto sphere = doi::Sphere{{}, {0.05f, 0.05f, 0.05f}};
		shape = doi::generateUV(sphere);
		spherePrimitiveID_ = scene_.addPrimitive(std::move(shape.positions),
			std::move(shape.normals), std::move(shape.indices));

		// init light visualizations
		doi::Material lmat;

		lmat.emissionFac = dirLight_.data.color;
		lmat.albedoFac = Vec4f(dirLight_.data.color);
		lmat.albedoFac[3] = 1.f;
		// HACK: make sure it doesn't write to depth buffer and isn't
		// rendered into shadow map
		lmat.flags |= doi::Material::Bit::blend;
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
		initTAA();

		// init layouts cb
		semaphoreInitLayouts_ = {dev};
		auto qf = dev.queueSubmitter().queue().family();
		cbInitLayouts_ = dev.commandAllocator().get(qf,
			vk::CommandPoolCreateBits::resetCommandBuffer);
	}

	void initPP() {
		auto& dev = vulkanDevice();

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

		auto ppBindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &nearestSampler_.vkHandle()),
		};

		pp_.dsLayout = {dev, ppBindings};
		pp_.pipeLayout = {dev, {{pp_.dsLayout.vkHandle()}}, {}};
		pp_.ds = {dev.descriptorAllocator(), pp_.dsLayout};

		vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
		vpp::ShaderModule fragShader(dev, taa_pp_frag_data);
		vpp::GraphicsPipelineInfo gpi {pp_.rp, pp_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}};

		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.logicOpEnable = false;
		pp_.pipe = {dev, gpi.info()};
	}

	void initTAA() {
		auto& dev = vulkanDevice();
		auto taaBindings = {
			// NOTE: not sure if linear or nearest sampler is better for access
			// into history
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, -1, 1, &linearSampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, -1, 1, &linearSampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, -1, 1, &linearSampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute),
		};

		taa_.dsLayout = {dev, taaBindings};
		taa_.ds = {dev.descriptorAllocator(), taa_.dsLayout};
		taa_.pipeLayout = {dev, {{taa_.dsLayout.vkHandle()}}, {}};

		vpp::ShaderModule compShader(device(), taa_taa_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = taa_.pipeLayout;
		cpi.stage.module = compShader;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;

		taa_.pipe = {dev, cpi};

		auto uboSize = sizeof(nytl::Mat4f) * 4 + sizeof(float) * 9;
		taa_.ubo = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// samples
#if 0 // Simple sample sequences
		taa_.samples = {
			// ARM
			Vec2f{-7.0f / 8.f, 1.0f / 8.f},
			Vec2f{-5.0f / 8.f, -5.0f / 8.f},
			Vec2f{-1.0f / 8.f, -3.0f / 8.f},
			Vec2f{3.0f / 8.f, -7.0f / 8.f},
			Vec2f{5.0f / 8.f, -1.0f / 8.f},
			Vec2f{7.0f / 8.f, 7.0f / 8.f},
			Vec2f{1.0f / 8.f, 3.0f / 8.f},
			Vec2f{-3.0f / 8.f, 5.0f / 8.f},

			// uniform4
			// Vec2f{-0.25f, -0.25f},
			// Vec2f{0.25f, 0.25f},
			// Vec2f{0.25f, -0.25f},
			// Vec2f{-0.25f, 0.25f},
		};
#endif

		// halton 16x
		constexpr auto len = 16;
		taa_.samples.resize(len);

		// http://en.wikipedia.org/wiki/Halton_sequence
		// index not zero based
		auto halton = [](int prime, int index = 1){
			float r = 0.0f;
			float f = 1.0f;
			int i = index;
			while (i > 0) {
				f /= prime;
				r += f * (i % prime);
				i = std::floor(i / (float)prime);
			}
			return r;
		};

		for (auto i = 0; i < len; i++) {
            float u = 2 * (halton(2, i + 1) - 0.5f);
            float v = 2 * (halton(3, i + 1) - 0.5f);
            taa_.samples[i] = {u, v};
			dlg_info("sample {}: {}", i, taa_.samples[i]);
        }
	}

	void initBuffers(const vk::Extent2D& size,
			nytl::Span<RenderBuffer> bufs) override {
		auto& dev = vulkanDevice();
		depthTarget() = createDepthTarget(size);

		for(auto& buf : bufs) {
			vk::FramebufferCreateInfo fbi({},
				pp_.rp, 1u, &buf.imageView.vkHandle(),
				size.width, size.height, 1);
			buf.framebuffer = {dev, fbi};
		}

		auto info = vpp::ViewableImageCreateInfo(historyFormat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::sampled | vk::ImageUsageBits::transferDst);
		dlg_assert(vpp::supported(dev, info.img));
		taa_.inHistory = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		info.img.usage = vk::ImageUsageBits::storage |
			vk::ImageUsageBits::transferSrc;
		dlg_assert(vpp::supported(dev, info.img));
		taa_.outHistory = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		info = vpp::ViewableImageCreateInfo(offscreenFormat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::sampled);
		dlg_assert(vpp::supported(dev, info.img));
		offscreen_.target = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
		auto attachments = {
			offscreen_.target.vkImageView(),
			depthTarget().vkImageView(),
		};
		auto fbi = vk::FramebufferCreateInfo({}, offscreen_.rp,
			attachments.size(), attachments.begin(),
			size.width, size.height, 1);
		offscreen_.fb = {dev, fbi};

		// descriptors
		vpp::DescriptorSetUpdate tdsu(taa_.ds);
		tdsu.imageSampler({{{}, taa_.inHistory.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		tdsu.storage({{{}, taa_.outHistory.vkImageView(),
			vk::ImageLayout::general}});
		tdsu.imageSampler({{{}, offscreen_.target.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		tdsu.imageSampler({{{}, depthTarget().vkImageView(),
			vk::ImageLayout::depthStencilReadOnlyOptimal}});
		tdsu.uniform({{{taa_.ubo}}});

		vpp::DescriptorSetUpdate pdsu(pp_.ds);
		// we can use inHistory here because the copy the data between
		// the compute shader and the post processing shader from
		// outHistory back to inHistory
		pdsu.imageSampler({{{}, taa_.inHistory.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		// TODO: we should this using initBufferLayouts_ and
		// the semaphore and wait when rendering instead of
		// stalling... not possible with current App architecture,
		// we e.g. need guarantees that after every initBuffers
		// updateDevice is called or something.
		// The stalling below is not nice
		vk::beginCommandBuffer(cbInitLayouts_, {});

		vk::ImageMemoryBarrier barrier;
		barrier.image = taa_.inHistory.image();
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.newLayout = vk::ImageLayout::transferDstOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferWrite;
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cbInitLayouts_,
			vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});
		vk::ClearColorValue cv {0.f, 0.f, 0.f, 1.f};
		vk::ImageSubresourceRange range{vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdClearColorImage(cbInitLayouts_, taa_.inHistory.image(),
			vk::ImageLayout::transferDstOptimal, cv, {{range}});

		barrier.oldLayout = vk::ImageLayout::transferDstOptimal;
		barrier.srcAccessMask = vk::AccessBits::transferWrite;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cbInitLayouts_,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier}});

		vk::endCommandBuffer(cbInitLayouts_);

		auto& qs = vulkanDevice().queueSubmitter();
		// vk::SubmitInfo submission;
		// submission.commandBufferCount = 1;
		// submission.pCommandBuffers = &cbInitLayouts_.vkHandle();
		// submission.signalSemaphoreCount = 1;
		// submission.pSignalSemaphores = &semaphoreInitLayouts_.vkHandle();
		qs.wait(qs.add(cbInitLayouts_));
	}

	bool features(doi::Features& enable, const doi::Features& supported) override {
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
		auto& defs = parser.definitions;
		auto it = std::find_if(defs.begin(), defs.end(),
			[](const argagg::definition& def){
				return def.name == "multisamples";
		});
		dlg_assert(it != defs.end());
		defs.erase(it);

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

	void record(const RenderBuffer& buf) override {
		auto cb = buf.commandBuffer;
		vk::beginCommandBuffer(cb, {});
		beforeRender(cb);

		auto [width, height] = swapchainInfo().imageExtent;
		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		// 1: render offscreeen
		std::array<vk::ClearValue, 2u> cv {};
		cv[0] = {{0.f, 0.f, 0.f, 0.f}}; // color
		cv[1].depthStencil = {1.f, 0u};
		vk::cmdBeginRenderPass(cb, {offscreen_.rp, offscreen_.fb,
			{0u, 0u, width, height},
			std::uint32_t(cv.size()), cv.data()}, {});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, false); // opaque

		vk::cmdBindIndexBuffer(cb, boxIndices_.buffer(),
			boxIndices_.offset(), vk::IndexType::uint16);
		doi::cmdBindGraphicsDescriptors(cb, env_.pipeLayout(), 0,
			{envCameraDs_});
		env_.render(cb);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, blendPipe_);
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, true); // transparent/blend

		vk::cmdEndRenderPass(cb);

		// 2: apply TAA
		// first make sure outHistory has the correct layout
		vk::ImageMemoryBarrier obarrier;
		obarrier.image = taa_.outHistory.image();
		obarrier.oldLayout = vk::ImageLayout::undefined;
		obarrier.newLayout = vk::ImageLayout::general;
		obarrier.dstAccessMask = vk::AccessBits::shaderWrite;
		obarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::computeShader, {}, {}, {}, {{obarrier}});

		auto cx = (width + 7) >> 3; // ceil(width / 8)
		auto cy = (height + 7) >> 3; // ceil(height / 8)
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, taa_.pipe);
		doi::cmdBindComputeDescriptors(cb, taa_.pipeLayout, 0, {taa_.ds});
		vk::cmdDispatch(cb, cx, cy, 1);

		// copy from outHistory back to inHistory for next frame
		// but we also use inHistory in the pp pass already
		// outHistory can be discarded after this
		vk::ImageMemoryBarrier ibarrier;
		ibarrier.image = taa_.inHistory.image();
		ibarrier.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		ibarrier.srcAccessMask = vk::AccessBits::shaderRead;
		ibarrier.newLayout = vk::ImageLayout::transferDstOptimal;
		ibarrier.dstAccessMask = vk::AccessBits::transferWrite;
		ibarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};

		obarrier.oldLayout = vk::ImageLayout::general;
		obarrier.newLayout = vk::ImageLayout::transferSrcOptimal;
		obarrier.srcAccessMask = vk::AccessBits::shaderWrite;
		obarrier.dstAccessMask = vk::AccessBits::transferRead;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer, {}, {}, {}, {{ibarrier, obarrier}});

		vk::ImageCopy copy;
		copy.extent = {width, height, 1};
		copy.srcSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
		copy.dstSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
		vk::cmdCopyImage(cb,
			taa_.outHistory.vkImage(), vk::ImageLayout::transferSrcOptimal,
			taa_.inHistory.vkImage(), vk::ImageLayout::transferDstOptimal,
			{{copy}});

		ibarrier.oldLayout = vk::ImageLayout::transferDstOptimal;
		ibarrier.srcAccessMask = vk::AccessBits::transferWrite;
		ibarrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		ibarrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{ibarrier}});

		// 3: post processing, apply to swapchain buffer
		vk::cmdBeginRenderPass(cb, {pp_.rp, buf.framebuffer,
			{0u, 0u, width, height}, 0u, nullptr}, {});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		doi::cmdBindGraphicsDescriptors(cb, pp_.pipeLayout, 0, {pp_.ds});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad triangle fan
		vk::cmdEndRenderPass(cb);

		afterRender(cb);
		vk::endCommandBuffer(cb);
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);
		if(mode_ & modeDirLight) {
			dirLight_.render(cb, shadowData_, scene_);
		}
		if(mode_ & modePointLight) {
			pointLight_.render(cb, shadowData_, scene_);
		}
	}

	void update(double dt) override {
		App::update(dt);
		time_ += dt;

		// movement
		auto kc = appContext().keyboardContext();
		if(kc) {
			doi::checkMovement(camera_, *kc, dt);
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
				dirLight_.data.flags ^= doi::lightFlagPcf;
				pointLight_.data.flags ^= doi::lightFlagPcf;
				updateLight_ = true;
				return true;
			case ny::Keycode::up:
				aoFac_ *= 1.1;
				updateAOParams_ = true;
				dlg_info("ao factor: {}", aoFac_);
				return true;
			case ny::Keycode::down:
				aoFac_ /= 1.1;
				updateAOParams_ = true;
				dlg_info("ao factor: {}", aoFac_);
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
			case ny::Keycode::k5: // toggle TAA
				taa_.mode = (taa_.mode + 1) % 3;
				break;
			default:
				break;
		}

		return false;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			doi::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
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
		if(camera_.update) {
			updateLight_ = true; // for directional light
		}

		// always update the camera matrix
		// update scene ubo
		{
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			// sample sequence from
			// community.arm.com/developer/tools-software/graphics/b/blog/posts/temporal-anti-aliasing
			auto [width, height] = swapchainInfo().imageExtent;

			using namespace nytl::vec::cw::operators;
			taa_.sampleID = (taa_.sampleID + 1) % taa_.samples.size();
			auto pixSize = Vec2f{1.f / width, 1.f / height};
			// range [-0.5f, 0.5f] pixel
			// we don't jitter one whole pixel, only half
			auto off = taa_.samples[taa_.sampleID] * pixSize;
			// auto off = 2 * samples[taa_.sampleID] * pixSize;
			if(taa_.mode == taaModePassthrough) { // disabed
				off = {0.f, 0.f};
			}

			auto proj = matrix(camera_);
			auto jitterMat = doi::translateMat({off.x, off.y, 0.f});
			auto jproj = jitterMat * proj;

			doi::write(span, jproj);
			doi::write(span, camera_.pos);
			doi::write(span, camera_.perspective.near);
			doi::write(span, camera_.perspective.far);
			camera_.update = false;
			map.flush();

			auto envMap = envCameraUbo_.memoryMap();
			auto envSpan = envMap.span();
			// TODO: not sure about using jitterMat here
			doi::write(envSpan, jitterMat * fixedMatrix(camera_));
			envMap.flush();

			auto taaMap = taa_.ubo.memoryMap();
			auto taaSpan = taaMap.span();
			doi::write(taaSpan, nytl::Mat4f(nytl::inverse(proj)));
			doi::write(taaSpan, taa_.lastProj);
			doi::write(taaSpan, proj);
			doi::write(taaSpan, nytl::Mat4f(nytl::inverse(taa_.lastProj)));
			doi::write(taaSpan, off);
			doi::write(taaSpan, taa_.lastJitter);
			doi::write(taaSpan, camera_.perspective.near);
			doi::write(taaSpan, camera_.perspective.far);
			doi::write(taaSpan, taa_.minFac);
			doi::write(taaSpan, taa_.maxFac);
			doi::write(taaSpan, taa_.mode);
			taaMap.flush();

			taa_.lastProj = proj;
			taa_.lastJitter = off;
		}

		auto semaphore = scene_.updateDevice(matrix(camera_));
		if(semaphore) {
			addSemaphore(semaphore, vk::PipelineStageBits::allGraphics);
			App::scheduleRerecord();
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
			doi::write(span, mode_);
			doi::write(span, aoFac_);
			doi::write(span, u32(env_.convolutionMipmaps()));
			map.flush();
		}
	}

	// only for visualizing the box/sphere
	nytl::Mat4f dirLightObjMatrix() {
		return doi::translateMat(-dirLight_.data.dir);
	}

	nytl::Mat4f pointLightObjMatrix() {
		return doi::translateMat(pointLight_.data.position);
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	bool needsDepth() const override { return true; }
	const char* name() const override { return "Temporal Anti aliasing"; }
	vpp::RenderPass createRenderPass() override { return {}; } // we use our own
	std::pair<vk::RenderPass, unsigned> rvgPass() const override {
		return {pp_.rp, 0u};
	}

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
	float aoFac_ {0.1f};

	static constexpr u32 modeDirLight = (1u << 0);
	static constexpr u32 modePointLight = (1u << 1);
	static constexpr u32 modeSpecularIBL = (1u << 2);
	static constexpr u32 modeIrradiance = (1u << 3);
	u32 mode_ {modePointLight | modeIrradiance | modeSpecularIBL};

	doi::Texture dummyTex_;
	bool moveLight_ {false};

	struct {
		vpp::RenderPass rp;
		vpp::ViewableImage target;
		vpp::Framebuffer fb;
	} offscreen_;

	static constexpr u32 taaModePassthrough = 0u;
	static constexpr u32 taaModeClipColor = 1u;
	static constexpr u32 taaModeReprDepthRej = 2u;

	struct {
		vpp::ViewableImage inHistory;
		vpp::ViewableImage outHistory;
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::SubBuffer ubo;
		unsigned sampleID {0};
		nytl::Mat4f lastProj = nytl::identity<4, float>();
		nytl::Vec2f lastJitter;

		float minFac {0.85};
		float maxFac {0.98};
		u32 mode {taaModePassthrough};

		std::vector<nytl::Vec2f> samples;
	} taa_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::RenderPass rp;
	} pp_;

	vpp::Semaphore semaphoreInitLayouts_ {};
	vpp::CommandBuffer cbInitLayouts_ {};

	bool anisotropy_ {}; // whether device supports anisotropy
	bool multiview_ {}; // whether device supports multiview
	bool depthClamp_ {}; // whether the device supports depth clamping
	bool multiDrawIndirect_ {};

	float time_ {};
	bool rotateView_ {false}; // mouseLeft down

	doi::Scene scene_; // no default constructor
	doi::Camera camera_ {};

	struct DirLight : public doi::DirLight {
		using doi::DirLight::DirLight;
		u32 instanceID;
		u32 materialID;
	};

	struct PointLight : public doi::PointLight {
		using doi::PointLight::PointLight;
		u32 instanceID;
		u32 materialID;
	};

	u32 cubePrimitiveID_ {};
	u32 spherePrimitiveID_ {};

	// light and shadow
	doi::ShadowData shadowData_;
	DirLight dirLight_;
	PointLight pointLight_;
	bool updateLight_ {true};

	doi::Environment env_;
	doi::Texture brdfLut_;

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
