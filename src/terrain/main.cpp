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

using namespace tkn::types;

class TerrainApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	struct UboData {
		nytl::Mat4f vp;
		nytl::Vec3f viewPos;
		float dt;
		nytl::Vec3f toLight;
		float _pad1;
		nytl::Mat4f vpInv;
	};

	// ersionParticle.comp
	struct Particle {
		nytl::Vec2f pos {};
		nytl::Vec2f vel {};
		float sediment {};
		float lifetime {};
	};

	// static constexpr vk::Extent2D heightmapSize = {4096, 4096};
	static constexpr vk::Extent2D heightmapSize = {1024, 1024};
	static constexpr auto heightmapFormat = vk::Format::r32Sfloat;

	static constexpr vk::Extent2D shadowmapSize = {1024, 1024};
	static constexpr auto shadowmapFormat = vk::Format::r16Sfloat;

	static constexpr auto offscreenFormat = vk::Format::r16g16b16a16Sfloat;

	static constexpr auto particleCount = 32 * 1024;

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

		// heightmap
		auto usage = vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled |
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
			vk::BufferUsageBits::transferDst;
		particles_ = {dev.bufferAllocator(), sizeof(Particle) * particleCount,
			particleUsage, dev.deviceMemoryTypes()};

		// upload data
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		Subdivider::InitData initSubd;
		subd_ = {initSubd, dev, fileWatcher_, shape.indices.size(), cb};

		auto stage3 = vpp::fillStaging(cb, vertices_, verts);
		auto stage4 = vpp::fillStaging(cb, indices_, inds);
		auto stage5 = vpp::fillStaging(cb, particles_, tkn::bytes(particles));

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
			erosionParticlePipe_ = {dev, "terrain/erosionParticle.comp", fileWatcher_};
			auto& dsu = erosionParticlePipe_.dsu();
			dsu(heightmap_);
			dsu(particles_);
			dsu(ubo_);
		}

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

		vk::SubpassDependency dep;
		dep.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dep.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		dep.dstStageMask = vk::PipelineStageBits::fragmentShader;
		dep.dstAccessMask = vk::AccessBits::shaderRead;
		dep.srcSubpass = 0u;
		dep.dstSubpass = vk::subpassExternal;
		rpi.dependencies.push_back(dep);

		pass0_.rp = {dev, rpi.info()};
		vpp::nameHandle(pass0_.rp, "pass0.rp");
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
		dsupp.seek(0, 0);
		dsupp(pass0_.colorTarget);
		dsupp(pass0_.depthTarget);
		dsupp(ubo_);
	}

	void update(double dt) override {
		Base::update(dt);

		cam_.update(swaDisplay(), dt);

		fileWatcher_.update();
		renderPipe_.update();
		updatePipe_.update();
		genPipe_.update();
		shadowPipe_.update();
		erosionParticlePipe_.update();
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

			uboMap_.flush();
		}

		bool rerec = false;
		rerec |= renderPipe_.updateDevice();
		rerec |= updatePipe_.updateDevice();
		rerec |= subd_.updateDevice();
		rerec |= ppPipe_.updateDevice();
		rerec |= erosionParticlePipe_.updateDevice();

		if(rerec && mainPipesLoaded()) {
			Base::scheduleRerecord();
		}

		bool regen = false;
		regen |= genPipe_.updateDevice();
		regen |= shadowPipe_.updateDevice();

		if(regen && genPipesLoaded()) {
			generateHeightmap();
		}
	}

	void beforeRender(vk::CommandBuffer cb) override {
		if(!mainPipesLoaded()) {
			return;
		}

		// erode
		auto heightmapSrc = tkn::SyncScope {
			vk::PipelineStageBits::topOfPipe,
			vk::ImageLayout::shaderReadOnlyOptimal,
			{},
		};
		auto heightmapDst = tkn::SyncScope::computeReadWrite();

		auto subres = vk::ImageSubresourceRange{
			vk::ImageAspectBits::color,
			0, vk::remainingMipLevels,
			0, 1};
		tkn::barrier(cb, heightmap_.image(), heightmapSrc, heightmapDst, subres);

		auto erodedx = tkn::ceilDivide(particleCount, 64);
		tkn::cmdBind(cb, erosionParticlePipe_);
		vk::cmdDispatch(cb, erodedx, 1, 1);

		heightmapSrc = heightmapDst;
		heightmapDst = tkn::SyncScope::fragmentSampled();
		heightmapDst.stages = vk::PipelineStageBits::allCommands;
		tkn::barrier(cb, heightmap_.image(), heightmapSrc, heightmapDst, subres);

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

		vk::cmdEndRenderPass(cb);
	}

	void render(vk::CommandBuffer cb) override {
		if(!mainPipesLoaded()) {
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
			bool(erosionParticlePipe_.pipe()) &&
			subd_.loaded();
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		cam_.aspect({w, h});
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	void generateHeightmap() {
		if(!genPipesLoaded()) {
			return;
		}

		// record
		vk::beginCommandBuffer(genCb_, {});

		vk::ImageMemoryBarrier barrier;
		barrier.image = heightmap_.image();
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0,
			vk::remainingMipLevels, 0, 1};
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.newLayout = vk::ImageLayout::general;
		barrier.srcAccessMask = {};
		barrier.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::cmdPipelineBarrier(genCb_, vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

		tkn::cmdBind(genCb_, genPipe_);
		auto dx = tkn::ceilDivide(heightmapSize.width, 8);
		auto dy = tkn::ceilDivide(heightmapSize.height, 8);
		vk::cmdDispatch(genCb_, dx, dy, 1);

		// generate mipmaps
		tkn::DownscaleTarget dtarget;
		dtarget.image = heightmap_.image();
		dtarget.format = heightmapFormat;
		dtarget.extent = {heightmapSize.width, heightmapSize.height, 1u};
		dtarget.srcScope = {
			vk::PipelineStageBits::computeShader,
			vk::ImageLayout::general,
			vk::AccessBits::shaderWrite
		};

		auto genLevels = vpp::mipmapLevels(heightmapSize) - 1;

		tkn::SyncScope dstScope {
			vk::PipelineStageBits::allCommands,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::AccessBits::shaderRead,
		};

		tkn::downscale(genCb_, dtarget, genLevels, &dstScope);

		// generate shadowmap
		barrier.image = shadowmap_.image();
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0,
			vk::remainingMipLevels, 0, 1};
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.newLayout = vk::ImageLayout::general;
		barrier.srcAccessMask = {};
		barrier.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::cmdPipelineBarrier(genCb_, vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

		tkn::cmdBind(genCb_, shadowPipe_);
		dx = tkn::ceilDivide(shadowmapSize.width, 8);
		dy = tkn::ceilDivide(shadowmapSize.height, 8);
		vk::cmdDispatch(genCb_, dx, dy, 1);

		barrier.oldLayout = vk::ImageLayout::general;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessBits::shaderWrite;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(genCb_, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{barrier}});

		vk::endCommandBuffer(genCb_);

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

	const char* name() const override { return "terrain"; }
	bool needsDepth() const override { return false; }

protected:
	tkn::ManagedGraphicsPipe renderPipe_;
	tkn::ManagedGraphicsPipe ppPipe_;
	tkn::ManagedComputePipe updatePipe_;
	tkn::ManagedComputePipe erosionParticlePipe_;

	tkn::ControlledCamera cam_;
	tkn::FileWatcher fileWatcher_;

	vpp::SubBuffer ubo_;
	vpp::SubBuffer vertices_;
	vpp::SubBuffer indices_;
	vpp::SubBuffer particles_;

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

	Subdivider subd_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<TerrainApp>(argc, argv);
}
