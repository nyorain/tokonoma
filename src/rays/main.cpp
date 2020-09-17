#include <tkn/singlePassApp.hpp>
#include <tkn/image.hpp>
#include <tkn/color.hpp>
#include <tkn/pipeline.hpp>
#include <tkn/texture.hpp>
#include <tkn/render.hpp>
#include <tkn/transform.hpp>
#include <tkn/levelView.hpp>
#include <tkn/timeWidget.hpp>
#include <tkn/util.hpp>
#include <tkn/bits.hpp>
#include <tkn/types.hpp>
using namespace tkn::types;

#include <vpp/pipeline.hpp>
#include <vpp/descriptor.hpp>
#include <vpp/queue.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/submit.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>

#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>
#include <dlg/dlg.hpp>

#include "shared.glsl"

// Passes:
// 1. compute: generate new sample rays (with bounces)
// 2. graphics: render those new samples into a cleared hdr framebuffer
// 3. compute: merge rendered framebuffer into hdr history (lerp)
// 4. graphics: render history onto swapchain, tonemap

class RaysApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	static constexpr auto sampleCount = 64 * 1024u;
	// static constexpr auto sampleCount = 16;

	// static constexpr auto sampleCount = 1024;
	static constexpr auto maxBounces = 6u; // XXX: defined again in rays.comp
	static constexpr auto renderFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto maskFormat = vk::Format::r8g8Snorm;
	static constexpr auto renderDownscale = 1u;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		dlg_assertm(samples() == vk::SampleCountBits::e1,
			"This application doesn't support multisampling");

		auto& dev = vkDevice();

		linearSampler_ = {dev, tkn::linearSamplerInfo()};
		nearestSampler_ = {dev, tkn::nearestSamplerInfo()};

		// ubo
		auto hostBits = dev.hostMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(UboData),
			vk::BufferUsageBits::uniformBuffer, hostBits};
		uboMap_ = ubo_.memoryMap();

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		initScene(cb);
		initCompute();
		initGfx();

		tss_ = {dev, {"rays/tss.comp"}, fileWatcher_,
			tkn::ComputePipeInfoProvider::create(linearSampler_)};

		initPP();
		initMask();

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		rvgInit();

		timeWidget_ = {rvgContext(), defaultFont()};
		timeWidget_.reset();
		timeWidget_.addTiming("mask");
		timeWidget_.addTiming("update");
		timeWidget_.addTiming("render");
		timeWidget_.addTiming("tss");
		timeWidget_.addTiming("pp");
		timeWidget_.complete();

		// halton 16x
		constexpr auto len = 16;
		samples_.resize(len);

		// http://en.wikipedia.org/wiki/Halton_sequence
		// index not zero based
		auto halton = [](int prime, int index = 1){
			float r = 0.0f;
			float f = 1.0f;
			int i = index;
			while(i > 0) {
				f /= prime;
				r += f * (i % prime);
				i = std::floor(i / (float)prime);
			}
			return r;
		};

		// samples in range [-1, +1] per dimension, unit square
		for (auto i = 0; i < len; i++) {
			float u = 2 * (halton(2, i + 1) - 0.5f);
			float v = 2 * (halton(3, i + 1) - 0.5f);
			samples_[i] = {u, v};
		}

		return true;
	}

	void initPP() {
		auto& dev = vkDevice();

		auto gpi = vpp::GraphicsPipelineInfo {};
		gpi.renderPass(renderPass());
		static auto atts = {tkn::noBlendAttachment()};
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.attachmentCount = atts.size();
		gpi.blend.pAttachments = atts.begin();
		pp_ = {dev, {"tkn/shaders/fullscreen.vert"}, {"rays/pp.frag"},
			fileWatcher_, tkn::GraphicsPipeInfoProvider::create(gpi, linearSampler_)};
	}

	void initMask() {
		auto& dev = vkDevice();

		auto pass0 = {0u};
		auto rpi = tkn::renderPassInfo({{maskFormat}}, {{pass0}});
		mask_.rp = {dev, rpi.info()};

		static vk::PipelineColorBlendAttachmentState batt = {
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

		auto gpi = vpp::GraphicsPipelineInfo {};
		gpi.renderPass(mask_.rp);
		gpi.assembly.topology = vk::PrimitiveTopology::lineList;
		gpi.rasterization.lineWidth = 1.f;
		gpi.blend.attachmentCount = 1u;
		gpi.blend.pAttachments = &batt;
		mask_.pipe = {dev, {"rays/mask.vert"}, {"rays/mask.frag"},
			fileWatcher_, tkn::GraphicsPipeInfoProvider::create(gpi)};

		auto& dsu = mask_.pipe.dsu();
		dsu(ubo_);
		dsu(bufs_.segments);
	}

	void initScene(vk::CommandBuffer cb) {
		auto& dev = vkDevice();
		auto devMem = dev.deviceMemoryTypes();

		// TODO: use common unit.
		float fac = 25.f;
		std::initializer_list<Light> lights = {
			{fac * tkn::blackbodyApproxRGB(4000), 0.05f, {-1.f, 1.f}},
			{1.f * tkn::blackbodyApproxRGB(3500), 0.1f, {2.0f, 1.8f}},
			{1.f * tkn::blackbodyApproxRGB(5500), 0.1f, {-2.f, -1.8f}},
			// {1 * tkn::blackbody(5000), 0.1f, {-2.f, 2.f}},
			// {fac * tkn::blackbody(7000), 0.1f, {2.f, -1.f}},
		};

		// NOTE: descriptions not exactly up to date.
		std::initializer_list<Material> mats {
			{{1.f, 1.f, 1.f}, 1.f, 0.f}, // white rough
			{{0.2f, 0.3f, 0.7f}, 0.2f, 0.f}, // red shiny
			{{0.7f, 0.8f, 0.7f}, 0.2f, 1.f}, // green metal
			{{0.7f, 0.6f, 0.8f}, 0.05f, 0.f}, // blue mirror
			{{0.8f, 0.8f, 0.6f}, 0.05f, 1.f}, // yellow metal mirror
			// {{1.0f, 1.0f, 1.f}, 0.01f, 0.f}, // white mirror
		};

		// u32 mat = 0;
		// u32 matBox = 0;
		std::vector<Segment> segs {
			// {{1, 1}, {2, 1}, 0},
			// {{2, 1}, {2, 2}, 1},
			// {{2, 2}, {1, 2}, 2},
			// {{1, 2}, {1, 1}, 3},
			// {{-1, -2}, {1, -2}, 4},

			{{3, -2}, {3, 2}, 0},
			{{3, 2}, {-1, 2}, 0},
			{{-1, 2}, {-1, 3}, 0},
			{{-1, 3}, {-3, 3}, 0},
			{{-3, 3}, {-3, -3}, 0},
			{{-3, -3}, {-1, -3}, 0},
			{{-1, -3}, {-1, -2}, 0},
			{{-1, -2}, {3, -2}, 0},

			// box
			{{-2, -1.2}, {2, -1.2}, 1},
			{{2, -1.2}, {2, 1.2}, 1},
			{{2, 1.2}, {-2, 1.2}, 1},
			{{-2, 1.2}, {-2, -1.2}, 1},
		};

		// create circle
#if 0
		{
			using nytl::constants::pi;
			auto nPoints = 128u;
			for(auto i = 0u; i < nPoints; ++i) {
				auto a0 = float(2 * pi * (i / float(nPoints)));
				auto a1 = float(2 * pi * ((i + 1) / float(nPoints)));
				auto p0 = Vec2f{std::cos(a0), std::sin(a0)};
				auto p1 = Vec2f{std::cos(a1), std::sin(a1)};
				segs.push_back({p0, p1, 1});
			}
		}
#endif // 0

		// make sure they are all allocated on one buffer/device if possible
		// vpp::SubBuffer::InitData initBufs {};
		// 1. create
		std::array<vpp::SubBuffer::InitData, 6> ibs;
		bufs_.lights = {ibs[0], dev.bufferAllocator(),
			lights.size() * sizeof(Light),
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferDst, devMem};
		bufs_.segments = {ibs[1], dev.bufferAllocator(),
			segs.size() * sizeof(Segment),
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferDst, devMem};
		bufs_.materials = {ibs[2], dev.bufferAllocator(),
			mats.size() * sizeof(Material),
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferDst, devMem};

		bufs_.raysCmd = {ibs[3], dev.bufferAllocator(),
			sizeof(vk::DrawIndirectCommand),
			vk::BufferUsageBits::indirectBuffer |
			vk::BufferUsageBits::transferDst |
			vk::BufferUsageBits::vertexBuffer, devMem};
		// auto maxVerts = lights.size() * sampleCount * maxBounces * 2;
		// maxVerts *= 3u; // TODO: for rects/triangles
		// maxVerts *= 6u; // TODO: no idea
		// maxVerts *= 64u; // TODO: for multi ray generation in higher bounces
		auto maxVerts = lights.size() * sampleCount * (maxBounces + 1) * sizeof(LightVertex) * 2;
		// maxVerts *= 32u; // TODO: just to be safe
		bufs_.vertices = {ibs[4], dev.bufferAllocator(), maxVerts,
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::vertexBuffer, devMem};

		// 2. init
		bufs_.lights.init(ibs[0]);
		bufs_.segments.init(ibs[1]);
		bufs_.materials.init(ibs[2]);
		bufs_.raysCmd.init(ibs[3]);
		bufs_.vertices.init(ibs[4]);

		dlg_assert(tkn::bytes(segs).size() == segs.size() * sizeof(*segs.begin()));
		dlg_debug(tkn::bytes(segs).size());

		vpp::fillDirect(cb, bufs_.lights, tkn::bytes(lights));
		vpp::fillDirect(cb, bufs_.segments, tkn::bytes(segs));
		vpp::fillDirect(cb, bufs_.materials, tkn::bytes(mats));

		vk::DrawIndirectCommand cmd {};
		cmd.instanceCount = 1;
		vpp::fillDirect(cb, bufs_.raysCmd, tkn::bytes(cmd));

		scene_.lights = lights;
		scene_.materials = mats;
		scene_.segments = segs;
	}

	void initCompute() {
		auto& dev = vkDevice();
		comp_ = {dev, {"rays/rays2.comp"}, fileWatcher_,
			tkn::ComputePipeInfoProvider::create(linearSampler_)};

		// noise
		// noise tex
		std::array<std::string, 64u> spaths;
		std::array<const char*, 64u> paths;
		for(auto i = 0u; i < 64; ++i) {
			spaths[i] = dlg::format("noise/LDR_RGBA_{}.png", i);
			paths[i] = spaths[i].data();
		}

		tkn::TextureCreateParams tcp;
		// TODO: fix blitting!
		// tcp.format = vk::Format::r8Unorm;
		tcp.format = vk::Format::r8g8b8a8Unorm;
		tcp.srgb = false; // TODO: not sure tbh
		noise_ = tkn::buildTexture(dev, tkn::loadImageLayers(paths), tcp);

		// ds
		auto& dsu = comp_.dsu();
		dsu(bufs_.segments);
		dsu(bufs_.materials);
		dsu(bufs_.lights);
		dsu(bufs_.raysCmd);
		dsu(bufs_.vertices);
		dsu(ubo_);
		// dsu(noise_.imageView());
	}

	void initGfx() {
		auto& dev = vkDevice();

		// renderPass
		std::array<vk::AttachmentDescription, 1> attachments;
		attachments[0].initialLayout = vk::ImageLayout::undefined;
		attachments[0].finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		attachments[0].format = renderFormat;
		attachments[0].loadOp = vk::AttachmentLoadOp::clear;
		attachments[0].storeOp = vk::AttachmentStoreOp::store;
		attachments[0].samples = vk::SampleCountBits::e1;

		vk::AttachmentReference colorRefs[1];
		colorRefs[0].attachment = 0;
		colorRefs[0].layout = vk::ImageLayout::colorAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = colorRefs;

		// make sure the results are availble for tss afterwards
		vk::SubpassDependency dependency;
		dependency.srcSubpass = 0;
		dependency.srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dependency.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
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
		gfx_.rp = {dev, rpi};

		// layouts
		/*
		static auto vertexInfo = tkn::PipelineVertexInfo {{
			{0, 0, vk::Format::r32g32Sfloat},
			{1, 1, vk::Format::r32g32b32a32Sfloat},
		}, {
			{0, sizeof(Vec2f), vk::VertexInputRate::vertex},
			{1, sizeof(Vec4f), vk::VertexInputRate::vertex},
		}};
		*/

		vpp::GraphicsPipelineInfo gpi;
		gpi.renderPass(gfx_.rp);
		// gpi.vertex = vertexInfo.info();
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		gpi.rasterization.lineWidth = 1.f;

		// additive color blending
		// we don't really care about alpha
		static vk::PipelineColorBlendAttachmentState blenda;
		blenda.alphaBlendOp = vk::BlendOp::add;
		blenda.srcAlphaBlendFactor = vk::BlendFactor::one;
		blenda.dstAlphaBlendFactor = vk::BlendFactor::zero;
		blenda.colorBlendOp = vk::BlendOp::add;
		blenda.srcColorBlendFactor = vk::BlendFactor::one;
		blenda.dstColorBlendFactor = vk::BlendFactor::one;
		blenda.colorWriteMask =
			vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a;
		blenda.blendEnable = true;
		gpi.blend.attachmentCount = 1;
		gpi.blend.pAttachments = &blenda;

		gfx_.pipe = {dev, {"rays/ray2.vert"}, {"rays/ray.frag"},
			fileWatcher_, tkn::GraphicsPipeInfoProvider::create(gpi)};

		// ds
		auto& dsu = gfx_.pipe.dsu();
		dsu(bufs_.vertices);
		dsu(ubo_);
	}

	void initBuffers(const vk::Extent2D& size,
			nytl::Span<RenderBuffer> bufs) override {
		Base::initBuffers(size, bufs);
		auto dsize = size;
		dsize.width = std::max(dsize.width >> renderDownscale, 1u);
		dsize.height = std::max(dsize.height >> renderDownscale, 1u);

		auto& dev = vkDevice();
		auto info = vpp::ViewableImageCreateInfo(renderFormat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::transferDst |
			vk::ImageUsageBits::storage);
		dlg_assert(vpp::supported(dev, info.img));
		history_ = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		info.img.extent = {dsize.width, dsize.height, 1u};
		info.img.usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled;
		dlg_assert(vpp::supported(dev, info.img));
		gfx_.target = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		// update framebuffer
		auto attachments = {gfx_.target.vkImageView()};
		auto fbi = vk::FramebufferCreateInfo({}, gfx_.rp,
			attachments.size(), attachments.begin(),
			dsize.width, dsize.height, 1);
		gfx_.fb = {dev, fbi};

		// mask
		info = vpp::ViewableImageCreateInfo(maskFormat,
			vk::ImageAspectBits::color, dsize,
			vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::colorAttachment);
		dlg_assert(vpp::supported(dev, info.img));

		mask_.target = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		{
			auto attachments = {mask_.target.vkImageView()};
			fbi = vk::FramebufferCreateInfo({}, mask_.rp,
				attachments.size(), attachments.begin(),
				dsize.width, dsize.height, 1);
			mask_.fb = {dev, fbi};
		}

		// update descriptors
		auto& tssDsu = tss_.dsu();
		tssDsu(gfx_.target);
		tssDsu(history_);
		tssDsu(mask_.target);
		tssDsu.apply(); // TODO

		auto& ppDsu = pp_.dsu();
		ppDsu(history_, vk::ImageLayout::general);
		ppDsu.apply(); // TODO

		// TODO: hack
		// should simply be added as dependency (via semaphore) to
		// the next render submission
		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		vk::ImageMemoryBarrier barrier;
		barrier.image = history_.image();
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.newLayout = vk::ImageLayout::transferDstOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferWrite;
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});
		vk::ClearColorValue cv {0.f, 0.f, 0.f, 1.f};
		vk::ImageSubresourceRange range{vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdClearColorImage(cb, history_.image(),
			vk::ImageLayout::transferDstOptimal, cv, {{range}});

		barrier.oldLayout = vk::ImageLayout::transferDstOptimal;
		barrier.srcAccessMask = vk::AccessBits::transferWrite;
		barrier.newLayout = vk::ImageLayout::general;
		barrier.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier}});

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));
	}

	bool pipesLoaded() const {
		return pp_.pipe() &&
			tss_.pipe() &&
			comp_.pipe() &&
			mask_.pipe.pipe() &&
			gfx_.pipe.pipe();
	}

	// yeah, we could use BufferMemoryBarriers in this function
	// but i'm too lazy rn. And my gpu/mesa impl doesn't care anyways
	void beforeRender(vk::CommandBuffer cb) override {
		Base::beforeRender(cb);
		if(!pipesLoaded()) {
			return;
		}

		timeWidget_.start(cb);
		auto [width, height] = swapchainInfo().imageExtent;
		auto dwidth = std::max(width >> renderDownscale, 1u);
		auto dheight = std::max(height >> renderDownscale, 1u);

		// generate mask
		{
			auto width = dwidth;
			auto height = dheight;

			vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
			vk::cmdSetViewport(cb, 0, 1, vp);
			vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

			std::array<vk::ClearValue, 1u> cv {};
			cv[0] = {{0.f, 0.f, 0.f, 0.f}};
			vk::cmdBeginRenderPass(cb, {mask_.rp, mask_.fb,
				{0u, 0u, width, height},
				std::uint32_t(cv.size()), cv.data()}, {});

			tkn::cmdBind(cb, mask_.pipe);

			auto pixelSize = nytl::Vec2f{1.f / width, 1.f / height};
			vk::cmdPushConstants(cb, mask_.pipe.pipeLayout(),
				vk::ShaderStageBits::vertex, 0, 8, &pixelSize);

			u32 down = 0u;
			vk::cmdPushConstants(cb, mask_.pipe.pipeLayout(),
				vk::ShaderStageBits::vertex, 8, 4, &down);
			vk::cmdDraw(cb, 2 * scene_.segments.size(), 1, 0, 0);

			down = 1u;
			vk::cmdPushConstants(cb, mask_.pipe.pipeLayout(),
				vk::ShaderStageBits::vertex, 8, 4, &down);
			vk::cmdDraw(cb, 2 * scene_.segments.size(), 1, 0, 0);

			vk::cmdEndRenderPass(cb);
		}

		timeWidget_.addTimestamp(cb);

		// reset vertex counter
		vk::DrawIndirectCommand cmd {};
		cmd.instanceCount = 1;
		vpp::fillDirect(cb, bufs_.raysCmd, tkn::bytes(cmd));

		// make sure the filling (counts as transfer) finished
		vk::MemoryBarrier barrier;
		barrier.srcAccessMask = vk::AccessBits::transferWrite;
		barrier.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader, {}, {{barrier}}, {}, {});

		// generate rays to render
		tkn::cmdBind(cb, comp_);

		u32 dy = sampleCount / 16;
		vk::cmdDispatch(cb, scene_.lights.size(), dy, 1);
		timeWidget_.addTimestamp(cb);

		// make sure the generated rays are visible to gfx pipe
		barrier.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		barrier.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::vertexAttributeRead |
			vk::AccessBits::indirectCommandRead;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::allGraphics, {}, {{barrier}}, {}, {});

		// render into offscreen buffer
		vk::Viewport vp{0.f, 0.f, (float) dwidth, (float) dheight, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, dwidth, dheight});

		std::array<vk::ClearValue, 1u> cv {};
		cv[0] = {{0.f, 0.f, 0.f, 0.f}};
		vk::cmdBeginRenderPass(cb, {gfx_.rp, gfx_.fb,
			{0u, 0u, dwidth, dheight},
			std::uint32_t(cv.size()), cv.data()}, {});

		tkn::cmdBind(cb, gfx_.pipe);
		tkn::cmdBindVertexBuffers(cb, {{bufs_.vertices}});

		// auto pixelSize = nytl::Vec2f{1.f / width, 1.f / height};
		// vk::cmdPushConstants(cb, gfx_.pipe.pipeLayout(),
		// 	vk::ShaderStageBits::vertex, 0, 8, &pixelSize);

		vk::cmdDrawIndirect(cb, bufs_.raysCmd.buffer(),
			bufs_.raysCmd.offset(), 1, 0);

		vk::cmdEndRenderPass(cb);
		timeWidget_.addTimestamp(cb);

		// apply tss
		// work group size of tss.comp is 8x8
		auto cx = (width + 7) >> 3; // ceil(width / 8)
		auto cy = (height + 7) >> 3; // ceil(height / 8)
		tkn::cmdBind(cb, tss_);
		vk::cmdDispatch(cb, cx, cy, 1);

		// make sure tss results are available for post processing
		vk::ImageMemoryBarrier ibarrier;
		ibarrier.image = history_.image();
		ibarrier.oldLayout = vk::ImageLayout::general;
		ibarrier.newLayout = vk::ImageLayout::general;
		ibarrier.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		ibarrier.dstAccessMask = vk::AccessBits::shaderRead;
		ibarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{ibarrier}});
		timeWidget_.addTimestamp(cb);
	}

	void render(vk::CommandBuffer cb) override {
		if(!pipesLoaded()) {
			return;
		}

		tkn::cmdBind(cb, pp_);
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad

		rvgContext().bindDefaults(cb);
		rvgWindowTransform().bind(cb);
		timeWidget_.draw(cb);
		timeWidget_.addTimestamp(cb);
		timeWidget_.finish(cb);
	}

	void update(double dt) override {
		Base::update(dt);

		fileWatcher_.update();
		next_ |= pp_.update();
		next_ |= tss_.update();
		next_ |= comp_.update();
		next_ |= gfx_.pipe.update();
		next_ |= mask_.pipe.update();

		if(rayTime_ != -1.f) {
			rayTime_ += dt;
		}

		if(next_ || run_ || rayTime_ >= 0.f) {
			next_ = false;
			Base::scheduleRedraw();
		}

		time_ += dt;
	}

	void updateDevice() override {
		Base::updateDevice();

		using namespace nytl::vec::cw::operators;
		auto sampleID = frameID_ % samples_.size();
		auto sample = samples_[sampleID];
		auto [width, height] = swapchainInfo().imageExtent;
		width = std::max(width >> renderDownscale, 1u);
		height = std::max(height >> renderDownscale, 1u);
		auto pixSize = Vec2f{1.f / width, 1.f / height};
		sample = sample * pixSize;
		// sample = {0.f, 0.f}; // TODO

		auto& data = tkn::as<UboData>(uboMap_);
		data.transform = matrix_;
		data.offset = view_.center - 0.5f * view_.size;
		data.size = view_.size;
		data.time = time_;
		data.frameID = ++frameID_;
		data.jitter = sample;
		data.rayTime = rayTime_;
		uboMap_.flush();

		if(updateLight_) {
			updateLight_ = false;

			// TODO: hack
			auto& dev = vkDevice();
			auto& qs = dev.queueSubmitter();
			auto qfam = qs.queue().family();
			auto cb = dev.commandAllocator().get(qfam);
			vk::beginCommandBuffer(cb, {});
			vpp::fillDirect(cb, bufs_.lights, tkn::bytes(scene_.lights));
			vk::endCommandBuffer(cb);
			qs.wait(qs.add(cb));
		}

		bool rerec = false;
		rerec |= pp_.updateDevice();
		rerec |= gfx_.pipe.updateDevice();
		rerec |= comp_.updateDevice();
		rerec |= tss_.updateDevice();
		rerec |= mask_.pipe.updateDevice();
		if(rerec) {
			Base::scheduleRerecord();
		}

		timeWidget_.updateDevice();
	}

	void updateMatrix() {
		auto wsize = windowSize();
		view_.size = tkn::levelViewSize(wsize.x / float(wsize.y), 15.f);
		matrix_ = tkn::levelMatrix(view_);
		updateView_ = true;
	}

	void resize(unsigned width, unsigned height) override {
		Base::resize(width, height);
		updateMatrix();

		// update time widget position
		auto pos = nytl::Vec2ui{width, height};
		pos.x -= 240;
		pos.y = 10;
		timeWidget_.move(nytl::Vec2f(pos));
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(ev.pressed && ev.keycode == swa_key_n) {
			next_ = true;
			return true;
		} else if(ev.pressed && ev.keycode == swa_key_r) {
			run_ = !run_;
			return true;
		} else if(ev.pressed && ev.keycode == swa_key_t) {
			if(swa_display_active_keyboard_mods(swaDisplay()) & swa_keyboard_mod_shift) {
				rayTime_ = -1.f;
			} else {
				rayTime_ = 0.f;
			}

			return true;
		}

		return false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		auto pos = tkn::windowToLevel(windowSize(), view_, {ev.x, ev.y});
		auto dpy = swaDisplay();
		auto& lights = scene_.lights;
		if(swa_display_mouse_button_pressed(dpy, swa_mouse_button_left)
				&& lights.size() > 0) {
			lights[0].pos = pos;
		} else if(swa_display_mouse_button_pressed(dpy, swa_mouse_button_right)
				&& lights.size() > 1) {
			lights[1].pos = pos;
		} else if(swa_display_mouse_button_pressed(dpy, swa_mouse_button_middle)
				&& lights.size() > 2) {
			lights[2].pos = pos;
		}

		updateLight_ = true;
	}

	const char* name() const override { return "rays"; }
	bool needsDepth() const override { return false; }

protected:
	tkn::LevelView view_ {};
	nytl::Mat4f matrix_;
	float time_ {};
	bool next_ {false};
	bool run_ {true};

	bool updateView_ {true};
	bool updateLight_ {false};
	vpp::Sampler linearSampler_;
	vpp::Sampler nearestSampler_;

	struct {
		std::vector<Light> lights;
		std::vector<Segment> segments;
		std::vector<Material> materials;
	} scene_;

	struct {
		vpp::SubBuffer segments;
		vpp::SubBuffer lights;
		vpp::SubBuffer materials;
		vpp::SubBuffer raysCmd;
		vpp::SubBuffer vertices;
	} bufs_;

	struct {
		tkn::ManagedGraphicsPipe pipe;
		vpp::RenderPass rp;
		vpp::Framebuffer fb;
		vpp::ViewableImage target;
	} gfx_;

	struct {
		tkn::ManagedGraphicsPipe pipe;
		vpp::RenderPass rp;
		vpp::ViewableImage target;
		vpp::Framebuffer fb;
	} mask_;

	tkn::ManagedGraphicsPipe pp_;
	tkn::ManagedComputePipe tss_;
	tkn::ManagedComputePipe comp_;

	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;

	tkn::FileWatcher fileWatcher_;
	tkn::TimeWidget timeWidget_;

	vpp::ViewableImage history_;
	vpp::ViewableImage noise_;
	unsigned frameID_ {};

	std::vector<Vec2f> samples_ {};

	float rayTime_ {-1.f};
};

int main(int argc, const char** argv) {
	return tkn::appMain<RaysApp>(argc, argv);
}

