#include "subd.hpp"
#include "atmosphere.hpp"

#include <tkn/singlePassApp.hpp>
#include <tkn/types.hpp>
#include <tkn/pipeline.hpp>
#include <tkn/formats.hpp>
#include <tkn/ccam.hpp>
#include <tkn/render.hpp>
#include <tkn/fswatch.hpp>
#include <tkn/bits.hpp>
#include <tkn/util.hpp>
#include <tkn/scene/shape.hpp>

#include <vpp/commandAllocator.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/debug.hpp>
#include <vpp/image.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>

#include <swa/swa.h>
#include <cstddef>

using namespace tkn::types;

// TODO: should be ported to tkn::App instead
class TerrainApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	struct UboData {
		nytl::Mat4f viewMtx;
		nytl::Mat4f projMtx;
		nytl::Mat4f viewProjMtx;

		nytl::Mat4f invViewMtx;
		nytl::Mat4f invProjMtx;
		nytl::Mat4f invViewProjMtx;

		nytl::Vec3f viewPos;
		float dt;
		nytl::Vec3f toLight;
		float time;

		nytl::Vec3f sunColor;
		u32 frameCounter;
		nytl::Vec3f ambientColor;
	};

	// ersionParticle.comp
	struct Particle {
		nytl::Vec2f pos {0.f, 0.f};
		nytl::Vec2f vel {1.f, 0.f};
		float sediment {0.f};
		float water {0.f};
		float erode {0.f};
		float _pad;
		nytl::Vec2f oldPos {0.f, 0.f};
	};

	// static constexpr vk::Extent2D heightmapSize = {4096, 4096};
	static constexpr vk::Extent2D heightmapSize = {2048, 2048};
	static constexpr auto heightmapFormat = vk::Format::r32Sfloat;

	static constexpr vk::Extent2D shadowmapSize = {1024, 1024};
	static constexpr auto shadowmapFormat = vk::Format::r16Sfloat;

	static constexpr auto offscreenFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto volumeFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto downscaleDepthFormat = vk::Format::r32Sfloat;

	static constexpr auto particleCount = 1024;
	static constexpr auto volumetricDownscale = 2u;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
		linearSampler_ = {dev, tkn::linearSamplerInfo()};
		nearestSampler_ = {dev, tkn::nearestSamplerInfo()};

		depthFormat_ = tkn::findDepthFormat(vkDevice());
		if(depthFormat_ == vk::Format::undefined) {
			throw std::runtime_error("No depth format supported");
		}

		// pass data
		initPass0();
		initPass1();
		initErodePass();

		// heightmap
		auto usage = vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::transferSrc |
			vk::ImageUsageBits::transferDst;
		auto heightmapInfo = vpp::ViewableImageCreateInfo(
			heightmapFormat, vk::ImageAspectBits::color, heightmapSize, usage);
		heightmapInfo.img.mipLevels = vpp::mipmapLevels(heightmapInfo.img.extent);
		heightmapInfo.view.subresourceRange.levelCount = heightmapInfo.img.mipLevels;

		heightmap_ = {dev.devMemAllocator(), heightmapInfo, dev.deviceMemoryTypes()};
		vpp::nameHandle(heightmap_, "heightmap");

		// shadowmap
		usage = vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled;
		auto shadowmapInfo = vpp::ViewableImageCreateInfo(
			shadowmapFormat, vk::ImageAspectBits::color, shadowmapSize, usage);
		shadowmap_ = {dev.devMemAllocator(), shadowmapInfo, dev.deviceMemoryTypes()};
		vpp::nameHandle(shadowmap_, "shadowmap");

		// vertex & index data
		auto shape = tkn::generateQuad(
			{0.f, 0.f, 0.f},
			{1.f, 0.f, 0.f},
			{0.f, 0.f, 1.f}
		);

		std::vector<nytl::Vec4f> vverts;
		for(auto& v : shape.positions) vverts.emplace_back(v);
		auto verts = tkn::bytes(vverts);
		auto inds = tkn::bytes(shape.indices);

		vertices_ = {dev.bufferAllocator(), verts.size(),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};
		indices_ = {dev.bufferAllocator(), inds.size(),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};
		ubo_ = {dev.bufferAllocator(), sizeof(UboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
		uboMap_ = ubo_.memoryMap();

		// particles
		auto particles = std::vector<Particle>(particleCount);
		auto particleUsage = vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::transferDst;
		erode_.particles = {dev.bufferAllocator(), sizeof(Particle) * particleCount,
			particleUsage, dev.deviceMemoryTypes()};

		// atmosphere
		atmosSem_ = {dev};
		atmosphere_ = {dev, fileWatcher_, AtmosphereDesc::earth(1000.f)};

		// upload data
		auto& qs = dev.queueSubmitter();
		updateCb_ = dev.commandAllocator().get(qs.queue().family(),
			vk::CommandPoolCreateBits::resetCommandBuffer);
		vk::beginCommandBuffer(updateCb_, {});

		Subdivider::InitData initSubd;
		subd_ = {initSubd, dev, fileWatcher_, shape.indices.size(), updateCb_};

		auto stage3 = vpp::fillStaging(updateCb_, vertices_, verts);
		auto stage4 = vpp::fillStaging(updateCb_, indices_, inds);
		auto stage5 = vpp::fillStaging(updateCb_, erode_.particles, tkn::bytes(particles));

		vk::endCommandBuffer(updateCb_);
		auto sid = qs.add(updateCb_);

		// graphics pipeline
		{
			vpp::GraphicsPipelineInfo gpi;
			gpi.renderPass(pass0_.rp);
			gpi.depthStencil.depthTestEnable = true;
			gpi.depthStencil.depthWriteEnable = true;
			gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
			// gpi.rasterization.cullMode = vk::CullModeBits::back;
			gpi.rasterization.cullMode = vk::CullModeBits::none;
			gpi.rasterization.polygonMode = vk::PolygonMode::fill;
			gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
			gpi.vertex = Subdivider::vertexInfo();
			gpi.multisample.rasterizationSamples = samples();

			auto gfxProvider = tkn::GraphicsPipeInfoProvider::create(gpi, linearSampler_);
			renderPipe_ = {dev, {"terrain/terrain.vert"}, {"terrain/terrain.frag"},
				fileWatcher_, std::move(gfxProvider)};

			auto& renderDsu = renderPipe_.dsu();
			renderDsu(ubo_);
			renderDsu(vertices_);
			renderDsu(indices_);
			renderDsu(heightmap_);
			renderDsu(shadowmap_);
		}

		// update pipeline
		{
			auto spec = tkn::SpecializationInfo::create(u32(Subdivider::updateWorkGroupSize));
			auto updateProvider = tkn::ComputePipeInfoProvider::create(
				std::move(spec), linearSampler_);
			updatePipe_ = {dev, "terrain/update.comp", fileWatcher_, {},
				std::move(updateProvider)};

			auto& updateDsu = updatePipe_.dsu();
			updateDsu(ubo_);
			updateDsu(vertices_);
			updateDsu(indices_);
			updateDsu(heightmap_);
			updateDsu(subd_.keys0());
			updateDsu(subd_.keys1());
		}

		// generation pipeline
		{
			genSem_ = vpp::Semaphore{dev};
			genCb_ = dev.commandAllocator().get(dev.queueSubmitter().queue().family(),
				vk::CommandPoolCreateBits::resetCommandBuffer);
			genPipe_ = {dev, {"terrain/gen.comp"}, fileWatcher_};
			genPipe_.dsu().set(heightmap_);

			auto shadowProvider = tkn::ComputePipeInfoProvider::create(linearSampler_);
			shadowPipe_ = {dev, {"terrain/shadowmarch.comp"}, fileWatcher_,
				std::move(shadowProvider)};

			auto& dsuShadow = shadowPipe_.dsu();
			dsuShadow(heightmap_);
			dsuShadow(shadowmap_);
			dsuShadow(ubo_);
		}

		// post-process pipe
		{
			vpp::GraphicsPipelineInfo gpi;
			gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
			gpi.renderPass(renderPass());
			gpi.multisample.rasterizationSamples = samples();

			auto ppProvider = tkn::GraphicsPipeInfoProvider::create(gpi, nearestSampler_);
			ppPipe_ = {dev, {"tkn/shaders/fullscreen.vert"}, {"terrain/pp.frag"},
				fileWatcher_, std::move(ppProvider)};

			// descriptor updated in initBuffers
		}

		// erosion pipeline
		{
			erode_.particleUpdatePipe = {dev, "terrain/erosionParticle.comp",
				fileWatcher_, {}, tkn::ComputePipeInfoProvider::create(linearSampler_)};
			auto& dsu = erode_.particleUpdatePipe.dsu();
			dsu(heightmap_);
			dsu(erode_.particles);
			dsu(ubo_);
		}

		// erosion particles apply to heightmap
		{
			static auto vertexInfo = tkn::PipelineVertexInfo{{
					{0, 0, vk::Format::r32g32Sfloat, offsetof(Particle, oldPos)},
					{1, 0, vk::Format::r32Sfloat, offsetof(Particle, erode)},
				}, {
					{0, sizeof(Particle), vk::VertexInputRate::vertex},
				}
			};

			static vk::PipelineColorBlendAttachmentState blend = {
				true,
				// color
				vk::BlendFactor::one, // src
				vk::BlendFactor::one, // dst
				vk::BlendOp::add,
				// alpha, don't care
				vk::BlendFactor::zero, // src
				vk::BlendFactor::zero, // dst
				vk::BlendOp::add,
				// color write mask
				vk::ColorComponentBits::r |
					vk::ColorComponentBits::g |
					vk::ColorComponentBits::b |
					vk::ColorComponentBits::a,
			};

			vpp::GraphicsPipelineInfo gpi;
			gpi.assembly.topology = vk::PrimitiveTopology::pointList;
			gpi.rasterization.polygonMode = vk::PolygonMode::point;
			gpi.renderPass(erode_.rp);
			gpi.vertex = vertexInfo.info();
			gpi.blend.attachmentCount = 1u;
			gpi.blend.pAttachments = &blend;

			auto provider = tkn::GraphicsPipeInfoProvider::create(gpi);
			erode_.particleErodePipe = {dev, {"terrain/erode.vert"},
				{"terrain/erode.frag"}, fileWatcher_, std::move(provider)};
		}

		// erosion particles debug render in viewport
		{
			static auto vertexInfo = tkn::PipelineVertexInfo{{
				{0, 0, vk::Format::r32g32Sfloat, offsetof(Particle, pos)},
				{1, 0, vk::Format::r32g32Sfloat, offsetof(Particle, vel)},
				{2, 0, vk::Format::r32Sfloat, offsetof(Particle, sediment)},
				{3, 0, vk::Format::r32Sfloat, offsetof(Particle, water)},
			}, {
				{0, sizeof(Particle), vk::VertexInputRate::vertex},
			}};

			vpp::GraphicsPipelineInfo gpi;
			gpi.assembly.topology = vk::PrimitiveTopology::pointList;
			gpi.rasterization.polygonMode = vk::PolygonMode::point;
			gpi.renderPass(pass0_.rp);
			gpi.multisample.rasterizationSamples = samples();
			gpi.vertex = vertexInfo.info();
			gpi.depthStencil.depthTestEnable = true;
			// important for scattering in post-process.
			gpi.depthStencil.depthWriteEnable = false;
			gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

			auto provider = tkn::GraphicsPipeInfoProvider::create(gpi, linearSampler_);
			erode_.particleRenderPipe = {dev, {"terrain/particle.vert"},
				{"terrain/particle.frag"}, fileWatcher_, std::move(provider)};

			auto& dsu = erode_.particleRenderPipe.dsu();
			dsu(heightmap_);
			dsu(ubo_);
		}

		// images
		vk::ImageViewCreateInfo ivi;
		ivi.image = heightmap_.image();
		ivi.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		ivi.format = heightmapFormat;
		ivi.viewType = vk::ImageViewType::e2d;
		erode_.heightmapView = {dev, ivi};

		// erosion framebuffer
		vk::FramebufferCreateInfo fbi;
		fbi.width = heightmapSize.width;
		fbi.height = heightmapSize.height;
		fbi.layers = 1u;
		fbi.attachmentCount = 1u;
		fbi.pAttachments = &erode_.heightmapView.vkHandle();
		fbi.renderPass = erode_.rp;
		erode_.fb = {dev, fbi};
		vpp::nameHandle(erode_.fb, "erode.fb");

		qs.wait(sid);
		return true;
	}

	void initPass0() {
		auto& dev = vkDevice();

		auto pass0 = {0u, 1u};
		auto rpi = tkn::renderPassInfo(
			{{offscreenFormat, depthFormat_}},
			{{{pass0}}});

		auto& dep = rpi.dependencies.emplace_back();
		dep.srcStageMask =
			vk::PipelineStageBits::colorAttachmentOutput |
			vk::PipelineStageBits::earlyFragmentTests;
		dep.srcAccessMask =
			vk::AccessBits::colorAttachmentWrite |
			vk::AccessBits::depthStencilAttachmentWrite;
		dep.dstStageMask =
			vk::PipelineStageBits::fragmentShader |
			vk::PipelineStageBits::computeShader;
		dep.dstAccessMask =
			vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		dep.srcSubpass = 0u;
		dep.dstSubpass = vk::subpassExternal;

		rpi.attachments[0].finalLayout = vk::ImageLayout::general;
		rpi.attachments[1].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

		pass0_.rp = {dev, rpi.info()};
		vpp::nameHandle(pass0_.rp, "pass0.rp");
	}

	void initPass1() {
		auto& dev = vkDevice();

		pass1_.downscalePipe = {dev, {"terrain/downscale.comp"}, fileWatcher_,
			tkn::ComputePipeInfoProvider::create(nearestSampler_)};
		pass1_.volumetricPipe = {dev, {"terrain/volume.comp"}, fileWatcher_};
		pass1_.upscalePipe = {dev, {"terrain/upscale.comp"}, fileWatcher_,
			tkn::ComputePipeInfoProvider::create(nearestSampler_)};
	}

	void initErodePass() {
		auto& dev = vkDevice();

		auto pass0 = {0u};
		auto rpi = tkn::renderPassInfo({{heightmapFormat}}, {{pass0}});
		// don't clear the heightmap
		rpi.attachments[0].loadOp = vk::AttachmentLoadOp::load;
		rpi.attachments[0].initialLayout = vk::ImageLayout::shaderReadOnlyOptimal;

		auto& depIn = rpi.dependencies.emplace_back();
		depIn.srcStageMask = vk::PipelineStageBits::computeShader;
		depIn.srcAccessMask = vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite;
		depIn.dstStageMask = vk::PipelineStageBits::allGraphics;
		depIn.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::vertexAttributeRead |
			vk::AccessBits::colorAttachmentRead |
			vk::AccessBits::colorAttachmentWrite;
		depIn.srcSubpass = vk::subpassExternal;
		depIn.dstSubpass = 0u;

		auto& depOut = rpi.dependencies.emplace_back();
		depOut.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		depOut.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		depOut.dstStageMask = vk::PipelineStageBits::allCommands;
		depOut.dstAccessMask = vk::AccessBits::shaderRead;
		depOut.srcSubpass = 0u;
		depOut.dstSubpass = vk::subpassExternal;

		erode_.rp = {dev, rpi.info()};
		vpp::nameHandle(pass0_.rp, "erode.rp");
	}

	void initBuffers(const vk::Extent2D& size, nytl::Span<RenderBuffer> bufs) override {
		Base::initBuffers(size, bufs);
		auto& dev = vkDevice();

		// depth
		auto usage = vk::ImageUsageBits::depthStencilAttachment |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo(depthFormat_,
			vk::ImageAspectBits::depth, size, usage);
		info.img.samples = samples();
		pass0_.depthTarget = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
		vpp::nameHandle(pass0_.depthTarget, "pass0.depthTarget");

		// color
		usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled;
		info = vpp::ViewableImageCreateInfo(offscreenFormat,
			vk::ImageAspectBits::color, size, usage);
		pass0_.colorTarget = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
		vpp::nameHandle(pass0_.colorTarget, "pass0.colorTarget");

		// pass1
		auto downscaledSize = size;
		downscaledSize.width >>= volumetricDownscale;
		downscaledSize.height >>= volumetricDownscale;

		// color
		usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled;
		info = vpp::ViewableImageCreateInfo(volumeFormat,
			vk::ImageAspectBits::color, downscaledSize, usage);
		pass1_.colorTarget = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
		vpp::nameHandle(pass1_.colorTarget, "pass1.colorTarget");

		// depth
		usage = vk::ImageUsageBits::storage | vk::ImageUsageBits::sampled;
		info = vpp::ViewableImageCreateInfo(downscaleDepthFormat,
			vk::ImageAspectBits::color, downscaledSize, usage);
		pass1_.depthTarget = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
		vpp::nameHandle(pass1_.depthTarget, "pass1.depthTarget");

		// fb
		auto attachments = {
			pass0_.colorTarget.vkImageView(),
			pass0_.depthTarget.vkImageView()
		};

		vk::FramebufferCreateInfo fbi ({},
			pass0_.rp,
			attachments.size(), attachments.begin(),
			size.width, size.height, 1);
		pass0_.fb = {dev, fbi};

		// update descriptors
		auto& dsupp = ppPipe_.dsu();
		dsupp(pass0_.colorTarget);
		dsupp(ubo_);

		auto& dsuDownscale = pass1_.downscalePipe.dsu();
		dsuDownscale(pass0_.depthTarget);
		dsuDownscale(pass1_.depthTarget);
		dsuDownscale(ubo_);

		auto& dsuVolumetric = pass1_.volumetricPipe.dsu();
		dsuVolumetric(pass1_.colorTarget, linearSampler_);
		dsuVolumetric(pass1_.depthTarget, nearestSampler_);
		dsuVolumetric(shadowmap_, linearSampler_);
		dsuVolumetric(heightmap_, linearSampler_);
		dsuVolumetric(atmosphere_.transmittanceLUT(), linearSampler_);
		dsuVolumetric(ubo_);
		dsuVolumetric(atmosphere_.ubo());

		auto& dsuUpscale = pass1_.upscalePipe.dsu();
		dsuUpscale(pass1_.depthTarget);
		dsuUpscale(pass1_.colorTarget);
		dsuUpscale(pass0_.depthTarget);
		dsuUpscale(pass0_.colorTarget);
		dsuUpscale(ubo_);

		// TODO: should not be here I guess. rework how that is handled.
		// maybe add 'beforeSubmit' or something?
		dsupp.apply();
		dsuDownscale.apply();
		dsuVolumetric.apply();
		dsuUpscale.apply();
	}

	void update(double dt) override {
		Base::update(dt);
		time_ += dt;

		if(!paused_) {
			dayTime_ += dt;
		}

		cam_.update(swaDisplay(), dt);

		fileWatcher_.update();
		renderPipe_.update();
		updatePipe_.update();
		genPipe_.update();
		shadowPipe_.update();
		erode_.particleUpdatePipe.update();
		erode_.particleRenderPipe.update();
		erode_.particleErodePipe.update();
		ppPipe_.update();
		subd_.update();
		atmosphere_.update();
		pass1_.upscalePipe.update();
		pass1_.downscalePipe.update();
		pass1_.volumetricPipe.update();

		Base::scheduleRedraw();
	}

	void updateDevice(double dt) override {
		App::updateDevice(dt);

		// make sure it does not get too large
		frameCounter_ = (frameCounter_ + 1) % 2048;

		{
			auto& data = tkn::as<UboData>(uboMap_);
			if(cam_.needsUpdate) {
				data.viewMtx = cam_.viewMatrix();
				data.projMtx = cam_.projectionMatrix();
				data.viewProjMtx = cam_.viewProjectionMatrix();

				data.invViewMtx = Mat4f(nytl::inverse(data.viewMtx));
				data.invProjMtx = Mat4f(nytl::inverse(data.projMtx));
				data.invViewProjMtx = Mat4f(nytl::inverse(data.viewProjMtx));

				cam_.needsUpdate = false;
				data.viewPos = cam_.position();
			}
			data.dt = dt;
			data.time = time_;
			data.frameCounter = frameCounter_;

			const float dayTimeN = std::fmod(0.1 * dayTime_, 1.0);
			data.toLight = toLight(dayTimeN);

			// TODO: compute from atmosphere. Update when atmosphere or
			// time changed.
			data.sunColor = Vec3f{1.f, 1.f, 1.f}; // Sky::computeSunColor(dayTimeN);
			data.ambientColor = Vec3f{0.4f, 0.5f, 0.8f}; // Sky::computeAmbientColor(dayTimeN);

			uboMap_.flush();
		}

		bool rerec = false;
		rerec |= renderPipe_.updateDevice();
		rerec |= updatePipe_.updateDevice();
		rerec |= subd_.updateDevice();
		rerec |= ppPipe_.updateDevice();

		rerec |= erode_.particleErodePipe.updateDevice();
		rerec |= erode_.particleRenderPipe.updateDevice();
		rerec |= erode_.particleUpdatePipe.updateDevice();

		rerec |= pass1_.upscalePipe.updateDevice();
		rerec |= pass1_.downscalePipe.updateDevice();
		rerec |= pass1_.volumetricPipe.updateDevice();

		bool regen = false;
		regen |= genPipe_.updateDevice();

		bool rshadow = shadowPipe_.updateDevice();
		regen |= rshadow;
		rerec |= rshadow;

		if(rerec && mainPipesLoaded() && generated_) {
			Base::scheduleRerecord();
		}

		if(regen && genPipesLoaded()) {
			// don't regenerate terrain every time the pipe changes,
			// only the first time. We do this so we don't reset the
			// erosion just because e.g. an include file changed (and
			// the generation itself did not change).
			// If needed, just trigger regeneraiton manually via the
			// shortcut
			generateHeightmap(!generated_);

			if(!generated_) {
				generated_ = true;
				Base::scheduleRerecord();
			}
		}

		atmosphere_.updateDevice();
		if(atmosphere_.changed() && atmosphere_.pipe().pipe()) {
			auto& cb = updateCb_;
			vk::beginCommandBuffer(cb, {});
			atmosphere_.compute(cb);
			vk::endCommandBuffer(cb);

			vk::SubmitInfo si;
			si.pCommandBuffers = &cb.vkHandle();
			si.commandBufferCount = 1u;
			si.pSignalSemaphores = &atmosSem_.vkHandle();
			si.signalSemaphoreCount = 1u;
			vkDevice().queueSubmitter().add(si);

			addSemaphore(atmosSem_, vk::PipelineStageBits::allGraphics);
			if(!atmosComputed_) {
				atmosComputed_ = true;
				Base::scheduleRerecord();
			}
		}
	}

	void beforeRender(vk::CommandBuffer cb) override {
		if(!mainPipesLoaded() || !generated_) {
			return;
		}

		if(shadowPipe_.pipe()) {
			computeShadowmap(cb);
		}

		// erode
		if(erodePipesLoaded() && !pauseErode_) {
			for(auto i = 0u; i < 64; ++i) {
				// 1: update particles
				auto erodedx = tkn::ceilDivide(particleCount, 64);
				tkn::cmdBind(cb, erode_.particleUpdatePipe);
				vk::cmdDispatch(cb, erodedx, 1, 1);

				// 2: apply erosion
				const auto [width, height] = heightmapSize;
				vk::cmdBeginRenderPass(cb, {
					erode_.rp, erode_.fb,
					{0u, 0u, width, height}, 0, nullptr,
				}, {});

				vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
				vk::cmdSetViewport(cb, 0, 1, vp);
				vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

				tkn::cmdBind(cb, erode_.particleErodePipe);
				tkn::cmdBindVertexBuffers(cb, {{erode_.particles}});
				vk::cmdDraw(cb, particleCount, 1, 0, 0);

				vk::cmdEndRenderPass(cb);
			}
		}

		// subdivide
		// TODO: we don't need to do this every frame.
		// This would likely profit from low priority async compute.
		subd_.resetCounter(cb);
		tkn::cmdBind(cb, updatePipe_);
		subd_.computeUpdate(cb);
		subd_.writeDispatch(cb);

		// pass0
		const auto [width, height] = swapchainInfo().imageExtent;
		auto cvs = {
			vk::ClearValue{0.f, 0.f, 0.f, 0.f},
			vk::ClearValue{1.f, 0u},
		};
		vk::cmdBeginRenderPass(cb, {
			pass0_.rp,
			pass0_.fb,
			{0u, 0u, width, height},
			std::uint32_t(cvs.size()), cvs.begin()
		}, {});

		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		tkn::cmdBind(cb, renderPipe_);
		subd_.draw(cb);

		if(erodePipesLoaded() && drawParticles_) {
			// debug draw particles
			tkn::cmdBind(cb, erode_.particleRenderPipe);
			tkn::cmdBindVertexBuffers(cb, {{erode_.particles}});
			vk::cmdDraw(cb, particleCount, 1, 0, 0);
		}

		vk::cmdEndRenderPass(cb);

		// pass1
		auto scopeDepthSrc = tkn::SyncScope::discard();
		auto scopeDepthDst = tkn::SyncScope::computeWrite();
		tkn::barrier(cb, pass1_.depthTarget.image(), scopeDepthSrc, scopeDepthDst);

		auto scopeColorLRSrc = tkn::SyncScope::discard();
		auto scopeColorLRDst = tkn::SyncScope::computeWrite();
		tkn::barrier(cb, pass1_.colorTarget.image(), scopeColorLRSrc, scopeColorLRDst);

		// downscale
		auto dgx = tkn::ceilDivide(width >> volumetricDownscale, 8);
		auto dgy = tkn::ceilDivide(height >> volumetricDownscale, 8);

		const u32 downscalePcr = (1u << volumetricDownscale);
		vk::cmdPushConstants(cb, pass1_.downscalePipe.pipeLayout(),
			vk::ShaderStageBits::compute, 0u, 4u, &downscalePcr);
		tkn::cmdBind(cb, pass1_.downscalePipe);
		vk::cmdDispatch(cb, dgx, dgy, 1);

		scopeDepthSrc = scopeDepthDst;
		scopeDepthDst = tkn::SyncScope::computeSampled();
		tkn::barrier(cb, pass1_.depthTarget.image(), scopeDepthSrc, scopeDepthDst);

		// volumetrics
		tkn::cmdBind(cb, pass1_.volumetricPipe);
		vk::cmdDispatch(cb, dgx, dgy, 1);

		scopeColorLRSrc = scopeColorLRDst;
		scopeColorLRDst = tkn::SyncScope::computeSampled();
		tkn::barrier(cb, pass1_.colorTarget.image(), scopeColorLRSrc, scopeColorLRDst);

		// upscale
		auto gx = tkn::ceilDivide(width, 8);
		auto gy = tkn::ceilDivide(height, 8);
		tkn::cmdBind(cb, pass1_.upscalePipe);
		vk::cmdDispatch(cb, gx, gy, 1);

		auto scopeColorSrc = tkn::SyncScope::computeReadWrite();
		auto scopeColorDst = tkn::SyncScope::fragmentSampled();
		tkn::barrier(cb, pass0_.colorTarget.image(), scopeColorSrc, scopeColorDst);
	}

	void render(vk::CommandBuffer cb) override {
		if(!mainPipesLoaded() || !generated_ || !atmosComputed_) {
			return;
		}

		tkn::cmdBind(cb, ppPipe_);
		vk::cmdDraw(cb, 4, 1, 0, 0);
	}

	bool genPipesLoaded() const {
		return bool(genPipe_.pipe()) &&
			bool(shadowPipe_.pipe());
	}

	bool mainPipesLoaded() const {
		return bool(renderPipe_.pipe()) &&
			bool(updatePipe_.pipe()) &&
			bool(ppPipe_.pipe()) &&
			bool(pass1_.downscalePipe.pipe()) &&
			bool(pass1_.volumetricPipe.pipe()) &&
			bool(pass1_.upscalePipe.pipe()) &&
			subd_.loaded();
	}

	bool erodePipesLoaded() const {
		return bool(erode_.particleErodePipe.pipe()) &&
			bool(erode_.particleUpdatePipe.pipe()) &&
			bool(erode_.particleRenderPipe.pipe());
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		cam_.aspect({w, h});
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	void generateHeightmap(bool regenTerrain) {
		if(!genPipesLoaded()) {
			return;
		}

		// record
		auto cb = genCb_.vkHandle();
		vk::beginCommandBuffer(cb, {});

		// terrain: optionally recreate, create heightmap
		{
			auto subres = vk::ImageSubresourceRange {vk::ImageAspectBits::color, 0,
				vk::remainingMipLevels, 0, 1};
			auto srcScope = tkn::SyncScope{
				vk::PipelineStageBits::topOfPipe,
				vk::ImageLayout::shaderReadOnlyOptimal,
				{}
			};

			if(regenTerrain) {
				srcScope.layout = vk::ImageLayout::undefined; // discard
				auto dstScope = tkn::SyncScope::computeReadWrite();
				tkn::barrier(cb, heightmap_.image(), srcScope, dstScope, subres);
				srcScope = dstScope;

				tkn::cmdBind(cb, genPipe_);
				auto dx = tkn::ceilDivide(heightmapSize.width, 8);
				auto dy = tkn::ceilDivide(heightmapSize.height, 8);
				vk::cmdDispatch(cb, dx, dy, 1);
			}

			// generate mipmaps
			tkn::DownscaleTarget dtarget;
			dtarget.image = heightmap_.image();
			dtarget.format = heightmapFormat;
			dtarget.extent = {heightmapSize.width, heightmapSize.height, 1u};
			dtarget.srcScope = srcScope;

			auto genLevels = vpp::mipmapLevels(heightmapSize) - 1;

			tkn::SyncScope dstScope {
				vk::PipelineStageBits::allCommands,
				vk::ImageLayout::shaderReadOnlyOptimal,
				vk::AccessBits::shaderRead,
			};

			tkn::downscale(cb, dtarget, genLevels, &dstScope);
		}

		computeShadowmap(cb);
		vk::endCommandBuffer(cb);

		// submit work
		vk::SubmitInfo si;
		si.pCommandBuffers = &genCb_.vkHandle();
		si.commandBufferCount = 1u;
		si.signalSemaphoreCount = 1u;
		si.pSignalSemaphores = &genSem_.vkHandle();

		auto& qs = vkDevice().queueSubmitter();
		qs.add(si);

		addSemaphore(genSem_, vk::PipelineStageBits::computeShader |
			vk::PipelineStageBits::vertexShader |
			vk::PipelineStageBits::fragmentShader);
	}

	void computeShadowmap(vk::CommandBuffer cb) {
		auto srcScope = tkn::SyncScope::discard();
		auto dstScope = tkn::SyncScope::computeReadWrite();
		tkn::barrier(cb, shadowmap_.image(), srcScope, dstScope);

		tkn::cmdBind(cb, shadowPipe_);
		auto dx = tkn::ceilDivide(shadowmapSize.width, 8);
		auto dy = tkn::ceilDivide(shadowmapSize.height, 8);
		vk::cmdDispatch(cb, dx, dy, 1);

		srcScope = dstScope;
		dstScope = tkn::SyncScope::fragmentSampled();
		tkn::barrier(cb, shadowmap_.image(), srcScope, dstScope);
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(ev.pressed) {
			if(ev.keycode == swa_key_r) {
				generateHeightmap(false);
				return true;
			} else if(ev.keycode == swa_key_t) {
				generateHeightmap(true);
				return true;
			} else if(ev.keycode == swa_key_p) {
				paused_ ^= true;
				return true;
			} else if(ev.keycode == swa_key_u) {
				pauseErode_ ^= true;
				Base::scheduleRerecord();
				return true;
			} else if(ev.keycode == swa_key_o) {
				drawParticles_ ^= true;
				Base::scheduleRerecord();
				return true;
			}
		}

		return false;
	}

	const char* name() const override { return "terrain"; }
	bool needsDepth() const override { return false; }

	nytl::Vec3f toLight(float dayTimeN) {
		using nytl::constants::pi;
		const float dayTime = 2 * pi * dayTimeN;
		const Vec3f toLight = Vec3f{std::sin(dayTime), -std::cos(dayTime), 0.0};
		return toLight;
	}

protected:
	tkn::ManagedGraphicsPipe renderPipe_;
	tkn::ManagedGraphicsPipe ppPipe_;
	tkn::ManagedComputePipe updatePipe_;

	tkn::ControlledCamera cam_;
	tkn::FileWatcher fileWatcher_;

	vpp::SubBuffer ubo_;
	vpp::SubBuffer vertices_;
	vpp::SubBuffer indices_;

	vpp::MemoryMapView uboMap_;

	vpp::Sampler linearSampler_;
	vpp::Sampler nearestSampler_;

	vpp::ViewableImage heightmap_;
	vpp::ViewableImage shadowmap_;
	tkn::ManagedComputePipe genPipe_;
	tkn::ManagedComputePipe shadowPipe_;
	vpp::CommandBuffer genCb_;
	vpp::Semaphore genSem_;

	// offscreen rendering
	struct {
		vpp::ViewableImage depthTarget;
		vpp::ViewableImage colorTarget;
		vpp::RenderPass rp;
		vpp::Framebuffer fb;
	} pass0_;

	// volumetric rendering
	struct {
		vpp::ViewableImage depthTarget;
		vpp::ViewableImage colorTarget;
		tkn::ManagedComputePipe downscalePipe;
		tkn::ManagedComputePipe volumetricPipe;
		// tkn::ManagedComputePipe blurPipe; // TODO: add bilateral blur
		tkn::ManagedComputePipe upscalePipe;
	} pass1_;

	// erosion
	struct {
		tkn::ManagedComputePipe particleUpdatePipe;
		tkn::ManagedGraphicsPipe particleErodePipe;
		tkn::ManagedGraphicsPipe particleRenderPipe;
		vpp::SubBuffer particles;
		vpp::ImageView heightmapView;
		vpp::Framebuffer fb; // for heightmap, particleErodePipe
		vpp::RenderPass rp;
	} erode_;

	bool generated_ {}; // whether the terrain was generated yet
	Subdivider subd_;
	double time_ {};
	double dayTime_ {};
	bool paused_ {true};
	bool pauseErode_ {true};
	bool drawParticles_ {false};
	u32 frameCounter_ {};

	vpp::CommandBuffer updateCb_;
	Atmosphere atmosphere_;
	vpp::Semaphore atmosSem_;
	bool atmosComputed_ {false};
};

int main(int argc, const char** argv) {
	return tkn::appMain<TerrainApp>(argc, argv);
}
