#include <tkn/singlePassApp.hpp>
#include <tkn/pipeline.hpp>
#include <tkn/ccam.hpp>
#include <tkn/util.hpp>
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
		float attrStrength;
	};

	struct Particle {
		Vec3f pos;
		float lifetime;
		Vec3f vel;
		float mass;
	};

	static constexpr auto numParticles = 1024 * 256;
	// static constexpr auto offscreenFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto offscreenFormat = vk::Format::r32g32b32a32Sfloat;

public:
	using Base = tkn::SinglePassApp;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
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
		ubo_ = {dev.bufferAllocator(), sizeof(UboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
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

			static vk::PipelineColorBlendAttachmentState blend = {
				true,
				// color
				vk::BlendFactor::srcAlpha, // src
				vk::BlendFactor::one, // dst
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

			vpp::GraphicsPipelineInfo gpi;
			gpi.assembly.topology = vk::PrimitiveTopology::pointList;
			gpi.rasterization.polygonMode = vk::PolygonMode::point;
			gpi.renderPass(offscreen_.rp);
			gpi.multisample.rasterizationSamples = samples();
			gpi.vertex = vertexInfo.info();
			gpi.depthStencil.depthTestEnable = false;
			gpi.depthStencil.depthWriteEnable = false;
			gpi.blend.attachmentCount = 1u;
			gpi.blend.pAttachments = &blend;

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

		// finish
		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		if(!particlesComputePipe_.pipe() || !particlesGfxPipe_.pipe()) {
			return;
		}

		tkn::cmdBind(cb, particlesComputePipe_);
		auto groups = tkn::ceilDivide(numParticles, 64);
		vk::cmdDispatch(cb, groups, 1, 1);

		// TODO: technically, we need a buffer mem barrier here

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

	void render(vk::CommandBuffer cb) override {
		if(ppPipe_.pipe() && particlesComputePipe_.pipe() && particlesGfxPipe_.pipe()) {
			tkn::cmdBind(cb, ppPipe_);
			vk::cmdDraw(cb, 4, 1, 0, 0);
		}
	}

	void update(double dt) override {
		Base::update(dt);

		cam_.update(swaDisplay(), dt);

		fswatch_.update();
		particlesComputePipe_.update();
		particlesGfxPipe_.update();
		ppPipe_.update();

		Base::scheduleRedraw();
	}

	void updateDevice(double dt) override {
		Base::updateDevice(dt);

		// update pipelines
		auto rerec = false;
		rerec |= particlesComputePipe_.updateDevice();
		rerec |= particlesGfxPipe_.updateDevice();
		rerec |= ppPipe_.updateDevice();
		if(rerec) {
			Base::scheduleRerecord();
		}

		// write ubo data
		auto uboData = reinterpret_cast<UboData*>(uboMap_.ptr());
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
		uboData->attrStrength = swa_display_mouse_button_pressed(swaDisplay(), swa_mouse_button_right) ? 1.f : 0.f;

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

		vk::FramebufferCreateInfo fbi;
		fbi.width = size.width;
		fbi.height = size.height;
		fbi.layers = 1u;
		fbi.renderPass = offscreen_.rp;
		fbi.attachmentCount = 1u;
		fbi.pAttachments = &offscreen_.image.vkImageView();
		offscreen_.fb = {dev, fbi};
		nameHandle(offscreen_.fb);

		auto& dsu = ppPipe_.dsu();
		dsu(offscreen_.image, noiseSampler_);

		Base::initBuffers(size, buffers);
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool mouseWheel(float dx, float dy) override {
		if(Base::mouseWheel(dx, dy)) {
			return true;
		}

		targetZ_ += 0.05 * dy;
		targetZ_ = std::max(targetZ_, 0.0001f);
		return true;
	}

	const char* name() const override { return "p3"; }
	bool needsDepth() const override { return false; }

protected:
	tkn::ManagedComputePipe particlesComputePipe_;
	tkn::ManagedGraphicsPipe particlesGfxPipe_;
	tkn::ManagedGraphicsPipe ppPipe_;

	vpp::ViewableImage curlNoise_;
	vpp::Sampler noiseSampler_;

	vpp::SubBuffer particlesBuffer_;
	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
	tkn::ControlledCamera cam_;

	tkn::FileWatcher fswatch_;

	float targetZ_ {1.f};

	struct {
		vpp::RenderPass rp;
		vpp::Framebuffer fb;
		vpp::ViewableImage image;
	} offscreen_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<P3App>(argc, argv);
}

