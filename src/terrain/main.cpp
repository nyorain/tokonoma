#include "subd.hpp"

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

class TerrainApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	struct UboData {
		nytl::Mat4f vp;
		nytl::Vec3f viewPos;
		float dt;
		nytl::Vec3f toLight;
		float time;
		nytl::Mat4f vpInv;
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

	static constexpr auto particleCount = 1024;

	static auto toLight() {
		return normalized(nytl::Vec3f{-0.5, 0.2, -0.6});
	}

public:
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
		linearSampler_ = {dev, tkn::linearSamplerInfo()};
		nearestSampler_ = {dev, tkn::nearestSamplerInfo()};

		// pass data
		initPass0();
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

		// upload data
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		Subdivider::InitData initSubd;
		subd_ = {initSubd, dev, fileWatcher_, shape.indices.size(), cb};

		auto stage3 = vpp::fillStaging(cb, vertices_, verts);
		auto stage4 = vpp::fillStaging(cb, indices_, inds);
		auto stage5 = vpp::fillStaging(cb, erode_.particles, tkn::bytes(particles));

		vk::endCommandBuffer(cb);
		auto sid = qs.add(cb);

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
			genPipe_ = {dev, "terrain/gen.comp", fileWatcher_};
			genPipe_.dsu().set(heightmap_);

			auto shadowProvider = tkn::ComputePipeInfoProvider::create(linearSampler_);
			shadowPipe_ = {dev, "terrain/shadowmarch.comp", fileWatcher_, {},
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
		pass0_.depthFormat = tkn::findDepthFormat(vkDevice());
		if(pass0_.depthFormat == vk::Format::undefined) {
			throw std::runtime_error("No depth format supported");
		}

		auto pass0 = {0u, 1u};
		auto rpi = tkn::renderPassInfo(
			{{offscreenFormat, pass0_.depthFormat}},
			{{{pass0}}});

		auto& dep = rpi.dependencies.emplace_back();
		dep.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dep.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		dep.dstStageMask = vk::PipelineStageBits::fragmentShader;
		dep.dstAccessMask = vk::AccessBits::shaderRead;
		dep.srcSubpass = 0u;
		dep.dstSubpass = vk::subpassExternal;

		pass0_.rp = {dev, rpi.info()};
		vpp::nameHandle(pass0_.rp, "pass0.rp");
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
		auto info = vpp::ViewableImageCreateInfo(pass0_.depthFormat,
			vk::ImageAspectBits::depth, size, usage);
		info.img.samples = samples();
		pass0_.depthTarget = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		// color
		usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::sampled;
		info = vpp::ViewableImageCreateInfo(offscreenFormat,
			vk::ImageAspectBits::color, size, usage);
		pass0_.colorTarget = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

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
		dsupp(pass0_.depthTarget);
		dsupp(ubo_);

		// TODO: should not be here I guess
		dsupp.apply();
	}

	void update(double dt) override {
		Base::update(dt);
		time_ += dt;

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

		Base::scheduleRedraw();
	}

	void updateDevice(double dt) override {
		App::updateDevice(dt);

		{
			auto& data = tkn::as<UboData>(uboMap_);
			if(cam_.needsUpdate) {
				cam_.needsUpdate = false;
				data.vp = cam_.viewProjectionMatrix();
				data.viewPos = cam_.position();
				data.vpInv = nytl::Mat4f(nytl::inverse(cam_.viewProjectionMatrix()));
			}
			data.toLight = toLight();
			data.dt = dt;
			data.time = time_;

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

		if(rerec && mainPipesLoaded() && generated_) {
			Base::scheduleRerecord();
		}

		bool regen = false;
		regen |= genPipe_.updateDevice();
		regen |= shadowPipe_.updateDevice();

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
	}

	void beforeRender(vk::CommandBuffer cb) override {
		if(!mainPipesLoaded() || !generated_) {
			return;
		}

		// erode
		if(erodePipesLoaded()) {
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

#ifdef DEBUG_DRAW_PARTICLES
		if(erodePipesLoaded()) {
			// debug draw particles
			tkn::cmdBind(cb, erode_.particleRenderPipe);
			tkn::cmdBindVertexBuffers(cb, {{erode_.particles}});
			vk::cmdDraw(cb, particleCount, 1, 0, 0);
		}
#endif // DEBUG_DRAW_PARTICLES

		vk::cmdEndRenderPass(cb);
	}

	void render(vk::CommandBuffer cb) override {
		if(!mainPipesLoaded() || !generated_) {
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

		// generate shadowmap
		{
			auto srcScope = tkn::SyncScope{
				vk::PipelineStageBits::topOfPipe,
				vk::ImageLayout::undefined,
				{}
			};
			auto dstScope = tkn::SyncScope::computeReadWrite();
			tkn::barrier(cb, shadowmap_.image(), srcScope, dstScope);

			tkn::cmdBind(genCb_, shadowPipe_);
			auto dx = tkn::ceilDivide(shadowmapSize.width, 8);
			auto dy = tkn::ceilDivide(shadowmapSize.height, 8);
			vk::cmdDispatch(genCb_, dx, dy, 1);

			srcScope = dstScope;
			dstScope = tkn::SyncScope::fragmentSampled();
			tkn::barrier(cb, shadowmap_.image(), srcScope, dstScope);
		}

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
			}
		}

		return false;
	}

	const char* name() const override { return "terrain"; }
	bool needsDepth() const override { return false; }

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
		vk::Format depthFormat;
		vpp::ViewableImage depthTarget;
		vpp::ViewableImage colorTarget;
		vpp::RenderPass rp;
		vpp::Framebuffer fb;
	} pass0_;

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
};

int main(int argc, const char** argv) {
	return tkn::appMain<TerrainApp>(argc, argv);
}
