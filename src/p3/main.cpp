#include <tkn/singlePassApp.hpp>
#include <tkn/pipeline.hpp>
#include <tkn/ccam.hpp>
#include <tkn/util.hpp>
#include <tkn/formats.hpp>
#include <tkn/image.hpp>
#include <tkn/texture.hpp>
#include <tkn/render.hpp>

#include <vpp/handles.hpp>
#include <vpp/debug.hpp>
#include <vpp/submit.hpp>
#include <vpp/formats.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/queue.hpp>
#include <vpp/vk.hpp>

#include <nytl/bytes.hpp>
#include <dlg/dlg.hpp>

#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <vui/textfield.hpp>

#include <random>

using namespace tkn::types;
using vpp::nameHandle;
using tkn::nameHandle;

#define nameHandle(x) nameHandle(x, #x)

class P3App : public tkn::SinglePassApp {
public:
	struct UboData {
		Mat4f vp;
		Vec3f camPos;
		float dt;
		Vec3f attrPos;
		float targetZ;
		Vec3f camAccel {0.f, 0.f, 0.f}; // written from shader, read on cpu
		float attrStrength;
	};

	struct Particle {
		Vec3f pos;
		float lifetime;
		Vec3f vel;
		float mass;
	};

	static constexpr auto numParticles = 1024 * 1024;
	static constexpr auto offscreenFormat = vk::Format::r16g16b16a16Unorm;
	// static constexpr auto offscreenFormat = vk::Format::r16g16b16a16Sfloat;
	// static constexpr auto offscreenFormat = vk::Format::r32g32b32a32Sfloat;

	static constexpr auto rastFormat = vk::Format::r32Uint;

public:
	using Base = tkn::SinglePassApp;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		rvgInit();

		cam_.near(-0.0001);

		auto& dev = vkDevice();
		// depthFormat_ = tkn::findDepthFormat(dev);

		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		tkn::WorkBatcher wb(dev);
		wb.cb = cb;

		tkn::TextureCreateParams tcp;
		tcp.mipLevels = 1u;
		auto initCurlNoise = createTexture(wb, tkn::loadImage("curlnoise.ktx"), {});
		curlNoise_ = initTexture(initCurlNoise, wb);
		nameHandle(curlNoise_);

		noiseSampler_ = {dev, tkn::linearSamplerInfo(vk::SamplerAddressMode::repeat)};
		nameHandle(noiseSampler_);

		nearestSampler_ = {dev, tkn::nearestSamplerInfo()};
		nameHandle(nearestSampler_);

		// offscreen render pass
		auto pass0i = {0u};
		auto rpi = tkn::renderPassInfo({{offscreenFormat}}, {{pass0i}});
		auto& dep = rpi.dependencies.emplace_back();
		dep.srcSubpass = 0u;
		dep.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		dep.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dep.dstSubpass = vk::subpassExternal;
		dep.dstAccessMask = vk::AccessBits::shaderRead;
		dep.dstStageMask = vk::PipelineStageBits::fragmentShader;

		offscreen_.rp = {vkDevice(), rpi.info()};

		// buffers
		// We also have a shader that writes into this buffer.
		ubo_ = {dev.bufferAllocator(), sizeof(UboData),
			vk::BufferUsageBits::uniformBuffer | vk::BufferUsageBits::storageBuffer,
			dev.hostMemoryTypes()};
		uboMap_ = ubo_.memoryMap();

		std::vector<Particle> particles(numParticles);

		std::mt19937 rgen;
		rgen.seed(std::time(nullptr));
		std::uniform_real_distribution<float> posDistr(-5.f, 5.f);
		std::uniform_real_distribution<float> lifetimeDistr(0.f, 1.f);
		std::uniform_real_distribution<float> massDistr(0.1f, 10.f);

		for(auto& p : particles) {
			p.pos = {posDistr(rgen), posDistr(rgen), posDistr(rgen)}; // TODO: random in range?
			p.vel = {0.f, 0.f, 0.f};
			p.lifetime = lifetimeDistr(rgen);
			p.mass = massDistr(rgen);
		}

		particlesBuffer_ = {dev.bufferAllocator(), particles.size() * sizeof(particles[0]),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst |
				vk::BufferUsageBits::vertexBuffer,
			dev.deviceMemoryTypes()};

		auto bufStage = vpp::fillStaging(cb, particlesBuffer_, nytl::bytes(particles));

		// compute pipe
		particlesComputePipe_ = {dev, {"p3/p3.comp"}, fswatch_};
		auto& compDsu = particlesComputePipe_.dsu();
		compDsu(ubo_);
		compDsu(particlesBuffer_);
		compDsu(curlNoise_, noiseSampler_);

		// graphics pipe
		{
			static auto vertexInfo = tkn::PipelineVertexInfo{{
				{0, 0, vk::Format::r32g32b32Sfloat, offsetof(Particle, pos)},
				{1, 0, vk::Format::r32Sfloat, offsetof(Particle, lifetime)},
				{2, 0, vk::Format::r32g32b32Sfloat, offsetof(Particle, vel)},
			}, {
				{0, sizeof(Particle), vk::VertexInputRate::vertex},
			}};

			vpp::GraphicsPipelineInfo gpi;
			gpi.assembly.topology = vk::PrimitiveTopology::pointList;
			gpi.rasterization.polygonMode = vk::PolygonMode::point;
			gpi.renderPass(offscreen_.rp);
			gpi.multisample.rasterizationSamples = samples();
			gpi.vertex = vertexInfo.info();
			gpi.depthStencil.depthTestEnable = false;
			gpi.depthStencil.depthWriteEnable = false;
			gpi.blend.attachmentCount = 1u;
			gpi.blend.pAttachments = &blend_;

			auto provider = tkn::GraphicsPipeInfoProvider::create(gpi);
			particlesGfxPipe_ = {dev, {"p3/p3.vert"}, {"p3/p3.frag"}, fswatch_,
				std::move(provider)};

			auto& gfxDsu = particlesGfxPipe_.dsu();
			gfxDsu(ubo_);
		}

		// post-process
		{
			vpp::GraphicsPipelineInfo gpi;
			gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
			gpi.rasterization.polygonMode = vk::PolygonMode::fill;
			gpi.renderPass(renderPass());
			gpi.depthStencil.depthTestEnable = false;
			gpi.depthStencil.depthWriteEnable = false;

			auto provider = tkn::GraphicsPipeInfoProvider::create(gpi);
			ppPipe_ = {dev, {"tkn/shaders/fullscreen.vert"}, {"p3/pp.frag"}, fswatch_,
				std::move(provider)};
		}

		// camera velocity pipe
		{
			camVelPipe_ = {dev, {"p3/camVel.comp"}, fswatch_};
			auto& dsu = camVelPipe_.dsu();
			dsu(ubo_);
			dsu(curlNoise_, noiseSampler_);
		}

		// compRast
		{
			compRast_.compPipe = {dev, {"p3/render.comp"}, fswatch_};
			auto& dsuComp = compRast_.compPipe.dsu();
			dsuComp(ubo_);
			dsuComp(particlesBuffer_);

			vpp::GraphicsPipelineInfo gpi;
			gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
			gpi.rasterization.polygonMode = vk::PolygonMode::fill;
			gpi.renderPass(renderPass());
			gpi.depthStencil.depthTestEnable = false;
			gpi.depthStencil.depthWriteEnable = false;

			auto provider = tkn::GraphicsPipeInfoProvider::create(gpi);
			compRast_.combinePipe = {dev, {"tkn/shaders/fullscreen.vert"}, {"p3/combine.frag"},
				fswatch_, std::move(provider)};
		}

		// finish
		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// init gui
		auto& gui = this->gui();

		using namespace vui::dat;
		auto pos = nytl::Vec2f {100.f, 0};
		auto& panel = gui.create<vui::dat::Panel>(pos, 300.f);

		auto createCheckbox = [&](auto* name, auto& value) -> decltype(auto) {
			auto& cb = panel.create<Checkbox>(name);
			cb.checkbox().set(value);
			cb.checkbox().onToggle = [&](auto& val) {
				value = val.checked();
			};
			return cb;
		};

		createCheckbox("Accelerate Camera", accelCam_);

		auto& spaceshipCb = panel.create<Checkbox>("Spaceship Camera").checkbox();
		spaceshipCb.set(spaceshipCam_);
		spaceshipCb.onToggle = [&](auto& cb) {
			spaceshipCam_ = cb.checked();
			updateCam();
		};
		updateCam();

		auto& blendCb = panel.create<Checkbox>("Blend OneMinusSrc").checkbox();
		blendCb.set(true);
		blendCb.onToggle = [&](auto& cb) {
			if(cb.checked()) {
				blend_.dstColorBlendFactor = vk::BlendFactor::oneMinusSrcAlpha;
			} else {
				blend_.dstColorBlendFactor = vk::BlendFactor::one;
			}

			particlesGfxPipe_.reload();
		};

		auto& compRastCb = panel.create<Checkbox>("Compute Render").checkbox();
		compRastCb.set(compRast_.use);
		compRastCb.onToggle = [&](auto& cb) {
			compRast_.use = cb.checked();
			Base::scheduleRerecord();
		};

		return true;
	}

	void updateCam() {
		tkn::CamMoveControls move;
		move.mult = 0.25f;
		move.slowMult = 0.05f;
		if(spaceshipCam_) {
			tkn::SpaceshipCamControls controls;
			controls.rotateFac = 0.05;
			controls.yawFriction = 10.f;
			controls.pitchFriction = 10.f;
			// controls.moveFriction = 1.f;
			controls.move = move;
			// controls.move.mult = 5.f;
			cam_.useSpaceshipControl(controls);
		} else {
			cam_.useFirstPersonControl({}, move);
		}
	}

	void beforeRender(vk::CommandBuffer cb) override {
		if(!particlesComputePipe_.pipe()) {
			return;
		}

		tkn::cmdBind(cb, particlesComputePipe_);
		auto groups = tkn::ceilDivide(numParticles, 128);
		vk::cmdDispatch(cb, groups, 1, 1);

		// TODO: technically, we need a buffer mem barrier here

		if(compRast_.use && compRast_.compPipe.pipe()) {
			auto bR = tkn::ImageBarrier{compRast_.imgR.image(), tkn::SyncScope::discard(), tkn::SyncScope::transferWrite()};
			auto bG = tkn::ImageBarrier{compRast_.imgG.image(), tkn::SyncScope::discard(), tkn::SyncScope::transferWrite()};
			auto bB = tkn::ImageBarrier{compRast_.imgB.image(), tkn::SyncScope::discard(), tkn::SyncScope::transferWrite()};
			tkn::barrier(cb, {{bR, bG, bB}});

			vk::ClearColorValue cc {};
			auto subres = vk::ImageSubresourceRange{vk::ImageAspectBits::color, 0, 1, 0, 1};
			vk::cmdClearColorImage(cb, bR.image, vk::ImageLayout::transferDstOptimal, cc, 1, subres);
			vk::cmdClearColorImage(cb, bG.image, vk::ImageLayout::transferDstOptimal, cc, 1, subres);
			vk::cmdClearColorImage(cb, bB.image, vk::ImageLayout::transferDstOptimal, cc, 1, subres);

			tkn::nextDst(bR, tkn::SyncScope::computeReadWrite());
			tkn::nextDst(bG, tkn::SyncScope::computeReadWrite());
			tkn::nextDst(bB, tkn::SyncScope::computeReadWrite());
			tkn::barrier(cb, {{bR, bG, bB}});

			tkn::cmdBind(cb, compRast_.compPipe);
			vk::cmdDispatch(cb, groups, 1, 1);

			tkn::nextDst(bR, tkn::SyncScope::fragmentSampled());
			tkn::nextDst(bG, tkn::SyncScope::fragmentSampled());
			tkn::nextDst(bB, tkn::SyncScope::fragmentSampled());
			tkn::barrier(cb, {{bR, bG, bB}});
		} else if(!compRast_.use && particlesGfxPipe_.pipe()) {
			auto [width, height] = swapchainInfo().imageExtent;

			auto cvs = {
				vk::ClearValue{0.f, 0.f, 0.f, 0.f},
				vk::ClearValue{1.f, 0u},
			};
			vk::cmdBeginRenderPass(cb, {
				offscreen_.rp,
				offscreen_.fb,
				{0u, 0u, width, height},
				std::uint32_t(cvs.size()), cvs.begin()
			}, {});

			vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
			vk::cmdSetViewport(cb, 0, 1, vp);
			vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

			tkn::cmdBindVertexBuffers(cb, {{particlesBuffer_}});
			tkn::cmdBind(cb, particlesGfxPipe_);
			vk::cmdDraw(cb, numParticles, 1, 0, 0);

			vk::cmdEndRenderPass(cb);
		}

		if(camVelPipe_.pipe()) {
			tkn::cmdBind(cb, camVelPipe_);
			vk::cmdDispatch(cb, 1, 1, 1);
		}
	}

	void render(vk::CommandBuffer cb) override {
		if(particlesComputePipe_.pipe() && particlesGfxPipe_.pipe()) {
			if(compRast_.use) {
				if(compRast_.combinePipe.pipe()) {
					tkn::cmdBind(cb, compRast_.combinePipe);
					vk::cmdDraw(cb, 4, 1, 0, 0);
				}
			} else {
				if(ppPipe_.pipe()) {
					tkn::cmdBind(cb, ppPipe_);
					vk::cmdDraw(cb, 4, 1, 0, 0);
				}
			}
		}

		gui().draw(cb);
	}

	void update(double dt) override {
		Base::update(dt);

		cam_.update(swaDisplay(), dt);

		fswatch_.update();
		particlesComputePipe_.update();
		particlesGfxPipe_.update();
		ppPipe_.update();
		camVelPipe_.update();

		compRast_.compPipe.update();
		compRast_.combinePipe.update();

		Base::scheduleRedraw();
	}

	void updateDevice(double dt) override {
		Base::updateDevice(dt);

		// update pipelines
		auto rerec = false;
		rerec |= particlesComputePipe_.updateDevice();
		rerec |= particlesGfxPipe_.updateDevice();
		rerec |= ppPipe_.updateDevice();
		rerec |= camVelPipe_.updateDevice();
		rerec |= compRast_.compPipe.updateDevice();
		rerec |= compRast_.combinePipe.updateDevice();
		if(rerec) {
			Base::scheduleRerecord();
		}

		uboMap_.invalidate();
		auto uboData = reinterpret_cast<UboData*>(uboMap_.ptr());

		// update camera position
		if(accelCam_ && cam_.spaceshipControl().has_value()) {
			// Vec3f& camVel = cam_.spaceshipControl().value()->con.move.moveVel;
			// camVel += uboData->camAccel;

			// adjust camera rotation based on velocity/curl
			auto last = normalized(lastCamVel_);
			auto now = normalized(uboData->camAccel);
			auto half = last + now;
			if(length(half) > 0.000001) {
				half = normalized(half);

				auto c = cross(half, last);
				tkn::Quaternion oriChange;
				oriChange.x = -c.x;
				oriChange.y = -c.y;
				oriChange.z = -c.z;
				oriChange.w = dot(last, half);

				auto rot = cam_.orientation();
				rot = normalized(normalized(oriChange) * rot);
				cam_.orientation(rot);
			}

			lastCamVel_ = uboData->camAccel;
		} else {
			uboData->camAccel = {};
			lastCamVel_ = {0.f, 0.f, 0.f};
		}

		// write ubo data
		uboData->vp = cam_.viewProjectionMatrix();
		uboData->camPos = cam_.position();
		uboData->dt = dt;
		uboData->targetZ = targetZ_;

		int x, y;
		swa_display_mouse_position(swaDisplay(), &x, &y);
		auto [w, h] = windowSize();

		auto invViewProj = Mat4f(nytl::inverse(cam_.viewProjectionMatrix()));
		float targetDepth = tkn::multPos(cam_.projectionMatrix(), Vec3f{0.f, 0.f, -targetZ_}).z;
		auto attrPos = tkn::multPos(invViewProj, Vec3f{
			-1.f + 2.f * float(x) / w,
			-1.f + 2.f * float(y) / h,
			targetDepth,
		});

		uboData->attrPos = attrPos;
		uboData->attrStrength = attracting_ ? 1.f : 0.f;

		uboMap_.flush();
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		cam_.aspect({w, h});
	}

	void initBuffers(const vk::Extent2D& size, nytl::Span<RenderBuffer> buffers) override {
		// resize offscreen target
		auto& dev = vkDevice();
		auto info = vpp::ViewableImageCreateInfo(offscreenFormat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::colorAttachment | vk::ImageUsageBits::sampled);
		offscreen_.image = {dev.devMemAllocator(), info};
		nameHandle(offscreen_.image);

		// info = vpp::ViewableImageCreateInfo(depthFormat_,
		// 	vk::ImageAspectBits::depth, size,
		// 	vk::ImageUsageBits::depthStencilAttachment);
		// offscreen_.depth = {dev.devMemAllocator(), info};
		// nameHandle(offscreen_.depth);

		vk::FramebufferCreateInfo fbi;
		fbi.width = size.width;
		fbi.height = size.height;
		fbi.layers = 1u;
		fbi.renderPass = offscreen_.rp;
		auto atts = {offscreen_.image.vkImageView()}; // , offscreen_.depth.vkImageView()};
		fbi.attachmentCount = atts.size();
		fbi.pAttachments = atts.begin();
		offscreen_.fb = {dev, fbi};
		nameHandle(offscreen_.fb);

		auto& dsu = ppPipe_.dsu();
		dsu(offscreen_.image, noiseSampler_);
		dsu.apply();

		// compRast
		info = vpp::ViewableImageCreateInfo(rastFormat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::storage | vk::ImageUsageBits::sampled | vk::ImageUsageBits::transferDst);
		compRast_.imgR = {dev.devMemAllocator(), info};
		compRast_.imgG = {dev.devMemAllocator(), info};
		compRast_.imgB = {dev.devMemAllocator(), info};
		nameHandle(compRast_.imgR);
		nameHandle(compRast_.imgG);
		nameHandle(compRast_.imgB);

		auto& dsuCompRast = compRast_.compPipe.dsu();
		dsuCompRast.skip(2);
		dsuCompRast(compRast_.imgR);
		dsuCompRast(compRast_.imgG);
		dsuCompRast(compRast_.imgB);
		dsuCompRast.apply();

		auto& dsuCombine = compRast_.combinePipe.dsu();
		dsuCombine(compRast_.imgR, nearestSampler_);
		dsuCombine(compRast_.imgG, nearestSampler_);
		dsuCombine(compRast_.imgB, nearestSampler_);
		dsuCombine.apply();

		Base::initBuffers(size, buffers);
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool mouseButton(const swa_mouse_button_event& ev) override {
		if(Base::mouseButton(ev)) {
			return true;
		}

		cam_.mouseButton(ev.button, ev.pressed);
		if(ev.button == swa_mouse_button_right) {
			attracting_ = ev.pressed;
		}

		return true;
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		cam_.key(ev.keycode, ev.pressed);
		return true;
	}

	bool mouseWheel(float dx, float dy) override {
		if(Base::mouseWheel(dx, dy)) {
			return true;
		}

		targetZ_ *= std::pow(1.05, dy);
		targetZ_ = std::max(targetZ_, 0.0001f);
		return true;
	}

	const char* name() const override { return "p3"; }
	bool needsDepth() const override { return false; }

protected:
	tkn::ManagedComputePipe particlesComputePipe_;
	tkn::ManagedGraphicsPipe particlesGfxPipe_;
	tkn::ManagedGraphicsPipe ppPipe_;

	tkn::ManagedComputePipe camVelPipe_;

	vpp::ViewableImage curlNoise_;
	vpp::Sampler noiseSampler_;
	vpp::Sampler nearestSampler_;

	vpp::SubBuffer particlesBuffer_;
	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
	tkn::ControlledCamera cam_;

	tkn::FileWatcher fswatch_;

	float targetZ_ {1.f};
	bool attracting_ {};

	struct {
		vpp::RenderPass rp;
		vpp::Framebuffer fb;
		vpp::ViewableImage image;
		// vpp::ViewableImage depth;
	} offscreen_;

	// compoute rasterizer data
	struct {
		bool use {};

		tkn::ManagedComputePipe compPipe;
		tkn::ManagedGraphicsPipe combinePipe;

		vpp::ViewableImage imgR;
		vpp::ViewableImage imgG;
		vpp::ViewableImage imgB;
	} compRast_;

	bool spaceshipCam_ {false};
	bool accelCam_ {false};
	Vec3f lastCamVel_ {};

	vk::PipelineColorBlendAttachmentState blend_ = {
		true,
		// color
		vk::BlendFactor::srcAlpha, // src
		vk::BlendFactor::oneMinusSrcAlpha, // dst
		vk::BlendOp::add,
		// alpha, don't care
		vk::BlendFactor::zero, // src
		vk::BlendFactor::one, // dst
		vk::BlendOp::add,
		// color write mask
		vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a,
	};

	// vk::Format depthFormat_ {};
};

int main(int argc, const char** argv) {
	return tkn::appMain<P3App>(argc, argv);
}

