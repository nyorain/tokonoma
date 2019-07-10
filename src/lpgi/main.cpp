// Light probe and spherical harmonics based simple global illumination
// Only first sketch of an implementation, not optimized. Still has
// quite some issues (see TODOs, some of them would be quite
// hard to fix, e.g. correct directional shadow).
// Real version should be implemented in deferred renderer.
// Based upon br

// ideas:
// - allow visualization of GI probes via spheres as in shv
// - add ability to save the rendered probes to file

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

#include <shaders/br.model.vert.h>
#include <shaders/lpgi.model.frag.h>
#include <shaders/lpgi.shProj.comp.h>

#include <optional>
#include <vector>
#include <string>

using namespace tkn::types;

class ViewApp : public tkn::App {
public:
	static constexpr auto probeSize = vk::Extent2D {128, 128};
	static constexpr auto probeFaceSize = probeSize.width * probeSize.height * 8;
	static constexpr auto maxProbeCount = 32u;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		camera_.perspective.near = 0.01f;
		camera_.perspective.far = 10.f;

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
			return false;
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

		tkn::Environment::InitData initEnv;
		env_.create(initEnv, batch, "convolution.ktx", "irradiance.ktx", sampler_);
		env_.createPipe(device(), cameraDsLayout_, renderPass(), 0u, samples());
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

		// pipeline layout consisting of all ds layouts and pcrs
		pipeLayout_ = {dev, {{
			cameraDsLayout_.vkHandle(),
			scene_.dsLayout().vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			shadowData_.dsLayout.vkHandle(),
			aoDsLayout_.vkHandle(),
		}}, {}};

		vpp::ShaderModule vertShader(dev, br_model_vert_data);
		vpp::ShaderModule fragShader(dev, lpgi_model_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples()};

		gpi.vertex = tkn::Scene::vertexInfo();
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

		gpi.depthStencil.depthWriteEnable = false;
		blendPipe_ = {dev, gpi.info()};

		gpi.depthStencil.depthWriteEnable = true;
		gpi.rasterization.frontFace = vk::FrontFace::clockwise;
		probe_.pipe = {dev, gpi.info()};

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

		// spherical harmonics coeffs texture for ao
		auto shFormat = vk::Format::r16g16b16a16Sfloat;
		vpp::ViewableImageCreateInfo info;
		info.img.extent = {maxProbeCount, 1, 1};
		info.img.imageType = vk::ImageType::e1d;
		info.img.usage = vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::transferSrc |
			vk::ImageUsageBits::transferDst |
			vk::ImageUsageBits::storage;
		info.img.arrayLayers = 1u;
		info.img.mipLevels = 1;
		info.img.tiling = vk::ImageTiling::optimal;
		info.img.format = shFormat;
		info.img.samples = vk::SampleCountBits::e1;
		info.img.arrayLayers = 9u;

		info.view.format = shFormat;
		info.view.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		info.view.subresourceRange.levelCount = 1;
		info.view.subresourceRange.layerCount = 9u;
		info.view.components = {}; // identity
		info.view.viewType = vk::ImageViewType::e1dArray;

		dlg_assert(vpp::supported(dev, info.img));
		shTex_ = {dev.devMemAllocator(), info};
		tmpShTex_ = {dev.devMemAllocator(), info};

		vk::ImageMemoryBarrier barrierSH;
		barrierSH.image = shTex_.image();
		barrierSH.srcAccessMask = {};
		barrierSH.dstAccessMask = vk::AccessBits::shaderRead;
		barrierSH.oldLayout = vk::ImageLayout::undefined;
		barrierSH.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrierSH.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		barrierSH.subresourceRange.layerCount = 9u;
		barrierSH.subresourceRange.levelCount = 1u;

		auto barrierTSH = barrierSH;
		barrierTSH.image = tmpShTex_.image();
		barrierTSH.newLayout = vk::ImageLayout::transferDstOptimal;
		barrierTSH.dstAccessMask = vk::AccessBits::transferWrite;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::fragmentShader |
			vk::PipelineStageBits::transfer |
			vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{barrierSH, barrierTSH}});

		vk::ClearColorValue black {0.f, 0.f, 0.f, 0.f};
		vk::cmdClearColorImage(cb, tmpShTex_.image(),
			vk::ImageLayout::transferDstOptimal, black,
			{{barrierTSH.subresourceRange}});

		barrierTSH.oldLayout = vk::ImageLayout::transferDstOptimal;
		barrierTSH.newLayout = vk::ImageLayout::general;
		barrierTSH.srcAccessMask = vk::AccessBits::transferWrite;
		barrierTSH.dstAccessMask =
			vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrierTSH}});


		auto aoUboSize = 5 * 4u;
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
		adsu.imageSampler({{{}, shTex_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		adsu.uniform({{{aoUbo_}}});
		adsu.apply();

		initProbePipe(batch);
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

		return true;
	}

	// TODO: probe pipes currently don't work for directional light,
	// since their shadow mapping depend on the camera (i.e. not
	// guaranteed to work for anything behind current camera).
	// we would somehow have to refresh the shadow map projection
	// (and re-render all cascades) for *each face* we render here.
	void initProbePipe(const tkn::WorkBatcher&) {
		auto& dev = vulkanDevice();

		// faces, ubo, ds
		// TODO: duplication with cameraUbo_ and cameraDs_
		// PERF: defer initialization for buffers
		auto camUboSize = sizeof(nytl::Mat4f) // proj matrix
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
			dsu.uniform({{{face.ubo}}});

			vpp::DescriptorSetUpdate edsu(face.envDs);
			edsu.uniform({{{face.envUbo}}});
		}

		probe_.depth = createDepthTarget(probeSize);

		auto info = vpp::ViewableImageCreateInfo(swapchainInfo().imageFormat,
			vk::ImageAspectBits::color, probeSize,
			vk::ImageUsageBits::colorAttachment | vk::ImageUsageBits::sampled);
		dlg_assert(vpp::supported(dev, info.img));
		probe_.color = {dev.devMemAllocator(), info};

		// TODO: we have to create an extra render pass if the window pass
		// uses multisampling. We don't need/want it here
		dlg_assert(samples() == vk::SampleCountBits::e1);

		auto attachments = {probe_.color.vkImageView(), probe_.depth.vkImageView()};
		vk::FramebufferCreateInfo fbi;
		fbi.attachmentCount = attachments.size();
		fbi.pAttachments = attachments.begin();
		fbi.width = probeSize.width;
		fbi.height = probeSize.height;
		fbi.layers = 1;
		fbi.renderPass = renderPass();
		probe_.fb = {dev, fbi};

		// spherical harmonics projection pipeline
		auto shBindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, -1, 1, &sampler_.vkHandle()), // input cubemap
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute), // output image
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute), // ubo, output coords
		};

		probe_.comp.dsLayout = {dev, shBindings};

		vk::PushConstantRange pcr;
		pcr.size = sizeof(u32);
		pcr.stageFlags = vk::ShaderStageBits::compute;
		probe_.comp.pipeLayout = {dev, {{probe_.comp.dsLayout.vkHandle()}},
			{{pcr}}};

		vpp::ShaderModule compShader(device(), lpgi_shProj_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = probe_.comp.pipeLayout;
		cpi.stage.module = compShader;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;

		probe_.comp.pipe = {dev, cpi};

		probe_.comp.ubo = {dev.bufferAllocator(), 16,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
		probe_.comp.ds = {dev.descriptorAllocator(), probe_.comp.dsLayout};

		vpp::DescriptorSetUpdate dsu(probe_.comp.ds);
		dsu.imageSampler({{{}, probe_.color.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.storage({{{}, tmpShTex_.vkImageView(), vk::ImageLayout::general}});
		dsu.uniform({{{probe_.comp.ubo}}});
		dsu.apply();

		// cb
		auto& qs = device().queueSubmitter();
		auto qfam = qs.queue().family();
		probe_.cb = device().commandAllocator().get(qfam);
		probe_.rerecord = true;
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

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);
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
		scene_.render(cb, pipeLayout_, false); // opaque

		vk::cmdBindIndexBuffer(cb, boxIndices_.buffer(),
			boxIndices_.offset(), vk::IndexType::uint16);
		tkn::cmdBindGraphicsDescriptors(cb, env_.pipeLayout(), 0,
			{envCameraDs_});
		env_.render(cb);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, blendPipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {cameraDs_});
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
			dirLight_.ds(), pointLight_.ds(), aoDs_});
		scene_.render(cb, pipeLayout_, true); // transparent/blend

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
			case ny::Keycode::r: // refresh all light probes
				refreshLightProbes();
				return true;
			case ny::Keycode::t: // add light probe
				addLightProbe();
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
			case ny::Keycode::left:
				maxProbeDist_ *= 1.1;
				updateAOParams_ = true;
				dlg_info("maxProbeDist: {}", maxProbeDist_);
				return true;
			case ny::Keycode::right:
				maxProbeDist_ /= 1.1;
				updateAOParams_ = true;
				dlg_info("maxProbeDist: {}", maxProbeDist_);
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
				dlg_info("Irradiance: {}", bool(mode_ & modeIrradiance));
				return true;
			case ny::Keycode::k5:
				mode_ ^= modeStaticAO;
				updateAOParams_ = true;
				dlg_info("Static AO: {}", bool(mode_ & modeStaticAO));
				return true;
			case ny::Keycode::k6:
				mode_ ^= modeAOAlbedo;
				updateAOParams_ = true;
				dlg_info("AO Albedo: {}", bool(mode_ & modeAOAlbedo));
				return true;
			default:
				break;
		}

		return false;
	}

	// tknng this multiple times can be used for light bounces
	void refreshLightProbes() {
		probe_.refresh = true;
	}

	void setupLightProbeRendering(u32 i, Vec3f pos) {
		auto map = probe_.comp.ubo.memoryMap();
		auto span = map.span();
		tkn::write(span, pos);
		tkn::write(span, i);
		map.flush();
		map = {};

		// update ubo for camera
		for(auto i = 0u; i < 6u; ++i) {
			auto map = probe_.faces[i].ubo.memoryMap();
			auto span = map.span();

			auto mat = tkn::cubeProjectionVP(pos, i);
			tkn::write(span, mat);
			tkn::write(span, pos);
			tkn::write(span, 0.01f); // near
			tkn::write(span, 30.f); // far
			map.flush();

			// fixed matrix, position irrelevant
			auto envMap = probe_.faces[i].envUbo.memoryMap();
			auto envSpan = envMap.span();
			tkn::write(envSpan, tkn::cubeProjectionVP({}, i));
			envMap.flush();
		}
	}

	void addLightProbe() {
		if(lightProbes_.size() == maxProbeCount) {
			dlg_error("Maximum number of light probes reached");
			return;
		}

		setupLightProbeRendering(lightProbes_.size(), camera_.pos);
		lightProbes_.push_back(camera_.pos);
		probe_.pending = true;
		updateAOParams_ = true;
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

	void recordProbeCb() {
		auto cb = probe_.cb.vkHandle();
		vk::beginCommandBuffer(probe_.cb, {});
		vk::Viewport vp{0.f, 0.f,
			(float) probeSize.width, (float) probeSize.height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, probeSize.width, probeSize.height});

		std::array<vk::ClearValue, 6u> cv {};
		cv[0] = {0.f, 0.f, 0.f, 0.f}; // color
		cv[1].depthStencil = {1.f, 0u}; // depth
		for(auto i = 0u; i < 6; ++i) {
			auto& face = probe_.faces[i];

			vk::cmdBeginRenderPass(cb, {renderPass(), probe_.fb,
				{0u, 0u, probeSize.width, probeSize.height},
				std::uint32_t(cv.size()), cv.data()
			}, {});

			// almost the same as the default output, we just have to
			// use a different pipe and descriptor set
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, probe_.pipe);
			tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {face.ds});
			tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
				dirLight_.ds(), pointLight_.ds(), aoDs_});
			scene_.render(cb, pipeLayout_, false); // opaque

			vk::cmdBindIndexBuffer(cb, boxIndices_.buffer(),
				boxIndices_.offset(), vk::IndexType::uint16);
			tkn::cmdBindGraphicsDescriptors(cb, env_.pipeLayout(), 0,
				{face.envDs});
			env_.render(cb);

			// TODO: we need a probe_.blendPipe here
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, probe_.pipe);
			tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {face.ds});
			tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 2, {
				dirLight_.ds(), pointLight_.ds(), aoDs_});
			scene_.render(cb, pipeLayout_, true); // transparent/blend

			vk::cmdEndRenderPass(cb);

			// make sure writing to color target has finished
			// also make sure reading/writing the shTex has finished
			vk::ImageMemoryBarrier barrierSH;
			barrierSH.image = tmpShTex_.image();
			barrierSH.subresourceRange.aspectMask = vk::ImageAspectBits::color;
			barrierSH.subresourceRange.layerCount = 9;
			barrierSH.subresourceRange.levelCount = 1;
			barrierSH.oldLayout = vk::ImageLayout::general;
			barrierSH.newLayout = vk::ImageLayout::general;
			barrierSH.srcAccessMask =
				vk::AccessBits::shaderRead |
				vk::AccessBits::shaderWrite;
			barrierSH.dstAccessMask =
				vk::AccessBits::shaderRead |
				vk::AccessBits::shaderWrite;

			vk::ImageMemoryBarrier colorBarrier;
			colorBarrier.image = probe_.color.image();
			colorBarrier.oldLayout = vk::ImageLayout::presentSrcKHR; // render pass
			colorBarrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
			colorBarrier.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
			colorBarrier.dstAccessMask = vk::AccessBits::shaderRead;
			colorBarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
			vk::cmdPipelineBarrier(cb,
				vk::PipelineStageBits::colorAttachmentOutput |
				vk::PipelineStageBits::computeShader,
				vk::PipelineStageBits::computeShader,
				{}, {}, {}, {{colorBarrier, barrierSH}});

			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute,
				probe_.comp.pipe);
			tkn::cmdBindComputeDescriptors(cb, probe_.comp.pipeLayout,
				0, {probe_.comp.ds});
			u32 iface = i;
			vk::cmdPushConstants(cb, probe_.comp.pipeLayout,
				vk::ShaderStageBits::compute, 0, sizeof(u32), &iface);
			vk::cmdDispatch(cb, 1, 1, 1);

			// make sure compute shader has finished
			colorBarrier.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
			colorBarrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal; // whatever
			colorBarrier.srcAccessMask = vk::AccessBits::shaderRead;
			colorBarrier.dstAccessMask = vk::AccessBits::colorAttachmentWrite;
			vk::cmdPipelineBarrier(cb,
				vk::PipelineStageBits::computeShader,
				vk::PipelineStageBits::colorAttachmentOutput,
				{}, {}, {}, {{colorBarrier}});
		}

		vk::ImageMemoryBarrier barrierTSH;
		barrierTSH.image = tmpShTex_.image();
		barrierTSH.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		barrierTSH.subresourceRange.layerCount = 9;
		barrierTSH.subresourceRange.levelCount = 1;
		barrierTSH.oldLayout = vk::ImageLayout::general;
		barrierTSH.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrierTSH.srcAccessMask =
			vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		barrierTSH.dstAccessMask = vk::AccessBits::transferRead;

		vk::ImageMemoryBarrier barrierSH;
		barrierSH.image = shTex_.image();
		barrierSH.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		barrierSH.subresourceRange.layerCount = 9;
		barrierSH.subresourceRange.levelCount = 1;
		barrierSH.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrierSH.newLayout = vk::ImageLayout::transferDstOptimal;
		barrierSH.srcAccessMask = vk::AccessBits::shaderRead;
		barrierSH.dstAccessMask = vk::AccessBits::transferWrite;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::fragmentShader |
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrierSH, barrierTSH}});

		// apply sh coords
		vk::ImageCopy copy;
		copy.extent = {maxProbeCount, 1, 1};
		copy.dstSubresource.aspectMask = vk::ImageAspectBits::color;
		copy.dstSubresource.layerCount = 9;
		copy.srcSubresource.aspectMask = vk::ImageAspectBits::color;
		copy.srcSubresource.layerCount = 9;
		vk::cmdCopyImage(cb,
			tmpShTex_.image(), vk::ImageLayout::transferSrcOptimal,
			shTex_.image(), vk::ImageLayout::transferDstOptimal,
			{{copy}});

		barrierSH.oldLayout = vk::ImageLayout::transferDstOptimal;
		barrierSH.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrierSH.srcAccessMask = vk::AccessBits::transferWrite;
		barrierSH.dstAccessMask = vk::AccessBits::shaderRead;

		barrierTSH.oldLayout = vk::ImageLayout::transferSrcOptimal;
		barrierTSH.newLayout = vk::ImageLayout::general;
		barrierTSH.srcAccessMask = vk::AccessBits::transferRead;
		barrierTSH.dstAccessMask =
			vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader |
			vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{barrierSH, barrierTSH}});

		vk::endCommandBuffer(probe_.cb);
	}

	void updateDevice() override {
		if(App::rerecord_) {
			probe_.rerecord = true;
		}

		// update scene ubo
		if(camera_.update) {
			camera_.update = false;
			// updateLights_ set to false below

			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			tkn::write(span, matrix(camera_));
			tkn::write(span, camera_.pos);
			tkn::write(span, camera_.perspective.near);
			tkn::write(span, camera_.perspective.far);
			map.flush();

			auto envMap = envCameraUbo_.memoryMap();
			auto envSpan = envMap.span();
			tkn::write(envSpan, fixedMatrix(camera_));
			envMap.flush();

			updateLight_ = true;
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

		if(probe_.pending) {
			if(probe_.rerecord) {
				recordProbeCb();
				probe_.rerecord = false;
			}

			// TODO: don't stall
			// but we also have to make sure this finishes before the
			// ao params are updated to include the new probe...
			auto& qs = vulkanDevice().queueSubmitter();
			qs.wait(qs.add(probe_.cb));
			probe_.pending = false;
		}

		// TODO: THE STALLING HERE IS HORRIBLE
		// the correct way is to allocate one ubo/ds etc (per face...)
		// per light probe and render them all at once. then, copying
		// tmpShTex once in the end is enough as well...
		// Then remove the reset/clear hack in shProj.comp as well
		if(probe_.refresh) {
			for(auto i = 0u; i < lightProbes_.size(); ++i) {
				setupLightProbeRendering(i, lightProbes_[i]);
				auto& qs = vulkanDevice().queueSubmitter();
				qs.wait(qs.add(probe_.cb));
			}
			probe_.refresh = false;
			dlg_info("refreshed");
		}

		if(updateAOParams_) {
			updateAOParams_ = false;
			auto map = aoUbo_.memoryMap();
			auto span = map.span();
			tkn::write(span, mode_);
			tkn::write(span, u32(lightProbes_.size()));
			tkn::write(span, aoFac_);
			tkn::write(span, maxProbeDist_);
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

	bool needsDepth() const override { return true; }
	const char* name() const override { return "Light probe GI"; }

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
	vpp::ViewableImage shTex_; // spherical harmonics coeffs
	vpp::ViewableImage tmpShTex_; // when updating the coeffs

	static constexpr u32 modeDirLight = (1u << 0);
	static constexpr u32 modePointLight = (1u << 1);
	static constexpr u32 modeSpecularIBL = (1u << 2);
	static constexpr u32 modeIrradiance = (1u << 3);
	static constexpr u32 modeStaticAO = (1u << 4);
	static constexpr u32 modeAOAlbedo = (1u << 5);
	u32 mode_ {modePointLight | modeIrradiance | modeStaticAO | modeAOAlbedo | modeSpecularIBL};
	float aoFac_ {0.1f};
	float maxProbeDist_ {1.f};
	bool updateAOParams_ {true};
	std::vector<Vec3f> lightProbes_;

	struct {
		vpp::ViewableImage color;
		vpp::ViewableImage depth;
		vpp::Framebuffer fb;
		vpp::Pipeline pipe;

		bool pending {};
		bool refresh {};
		bool rerecord {};
		vpp::CommandBuffer cb;

		// compute
		struct {
			vpp::TrDsLayout dsLayout;
			vpp::PipelineLayout pipeLayout;
			vpp::TrDs ds;
			vpp::Pipeline pipe;
			vpp::SubBuffer ubo;
		} comp;

		struct Face {
			vpp::TrDs ds;
			vpp::SubBuffer ubo;

			vpp::TrDs envDs;
			vpp::SubBuffer envUbo;
		};

		std::array<Face, 6u> faces;
	} probe_ {};

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
