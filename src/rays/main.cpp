#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/render.hpp>
#include <tkn/transform.hpp>
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

#include <ny/key.hpp>
#include <ny/event.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/mat.hpp>
#include <dlg/dlg.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/rays.rays.comp.h>
#include <shaders/rays.ray.vert.h>
#include <shaders/rays.ray.frag.h>
#include <shaders/rays.tss.comp.h>
#include <shaders/rays.pp.frag.h>

// Passes:
// 1. compute: generate new sample rays (with bounces)
// 2. graphics: render those new samples into a cleared hdr framebuffer
// 3. compute: merge rendered framebuffer into hdr history (lerp)
// 4. graphics: render history onto swapchain, tonemap

struct Light {
	Vec3f color;
	float radius;
	Vec2f pos;
	float _pad0[2] {};
};

struct Material {
	Vec3f albedo;
	float roughness;
	float metallic;
	float _pad0[3] {};
};

struct Segment {
	Vec2f start;
	Vec2f end;
	u32 material;
	u32 _pad0 {};
};

class RaysApp : public tkn::App {
public:
	static constexpr auto sampleCount = 2048u;
	static constexpr auto maxBounces = 8u; // XXX: defined again in rays.comp
	static constexpr auto renderFormat = vk::Format::r16g16b16a16Sfloat;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		return true;
	}

	void initRenderData() override {
		dlg_assertm(samples() == vk::SampleCountBits::e1,
			"This application doesn't support multisampling");

		auto& dev = vulkanDevice();
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

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		initScene(cb);
		initCompute();
		initGfx();
		initTss();
		initPP();

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));
	}

	void initTss() {
		auto& dev = vulkanDevice();

		// layouts
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		tss_.dsLayout = {dev, bindings};
		tss_.pipeLayout = {dev, {{tss_.dsLayout.vkHandle()}}, {}};

		// pipe
		vpp::ShaderModule module(dev, rays_tss_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = tss_.pipeLayout;
		cpi.stage.module = module;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		tss_.pipe = {dev, cpi};

		// ds
		tss_.ds = {dev.descriptorAllocator(), tss_.dsLayout};
	}

	void initPP() {
		auto& dev = vulkanDevice();

		// layouts
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		pp_.dsLayout = {dev, bindings};
		pp_.pipeLayout = {dev, {{pp_.dsLayout.vkHandle()}}, {}};

		// pipe
		auto vert = vpp::ShaderModule(dev, tkn_fullscreen_vert_data);
		auto frag = vpp::ShaderModule(dev, rays_pp_frag_data);

		vpp::GraphicsPipelineInfo gpi(renderPass(), pp_.pipeLayout, {{{
				{vert, vk::ShaderStageBits::vertex},
				{frag, vk::ShaderStageBits::fragment}
		}}}, 0u);

		auto atts = {tkn::noBlendAttachment()};
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.attachmentCount = atts.size();
		gpi.blend.pAttachments = atts.begin();
		pp_.pipe = {dev, gpi.info()};

		// ds
		pp_.ds = {dev.descriptorAllocator(), pp_.dsLayout};
	}

	void initScene(vk::CommandBuffer cb) {
		auto& dev = vulkanDevice();
		auto devMem = dev.deviceMemoryTypes();

		// float fac = 10.f;
		std::initializer_list<Light> lights = {
			{20 * tkn::blackbody(3500), 0.1f, {-1.f, 1.f}},
			// {1 * tkn::blackbody(5500), 0.1f, {2.0f, 1.8f}},
			// {fac * tkn::blackbody(5500), 0.1f, {-2.f, -1.8f}},
			// {1 * tkn::blackbody(5000), 0.1f, {-2.f, 2.f}},
			// {fac * tkn::blackbody(7000), 0.1f, {2.f, -1.f}},
		};

		std::initializer_list<Material> mats {
			{{0.5f, 0.5f, 0.5f}, 1.f, 0.f}, // white rough
			{{0.8f, 0.7f, 0.7f}, 0.2f, 0.f}, // red shiny
			{{0.7f, 0.8f, 0.7f}, 0.2f, 1.f}, // green metal
			{{0.7f, 0.6f, 0.8f}, 0.05f, 0.f}, // blue mirror
			{{0.8f, 0.8f, 0.6f}, 0.05f, 1.f}, // yellow metal mirror
			// {{1.0f, 1.0f, 1.f}, 0.01f, 0.f}, // white mirror
		};

		u32 mat = 0;
		u32 matBox = 0;
		std::initializer_list<Segment> segs {
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
			{{-2, -1.2}, {2, -1.2}, 0},
			{{2, -1.2}, {2, 1.2}, 0},
			{{2, 1.2}, {-2, 1.2}, 0},
			{{-2, 1.2}, {-2, -1.2}, 0},
		};

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
		auto maxVerts = lights.size() * sampleCount * maxBounces * 2;
		bufs_.positions = {ibs[4], dev.bufferAllocator(),
			maxVerts * sizeof(Vec2f),
			vk::BufferUsageBits::storageBuffer, devMem};
		bufs_.colors = {ibs[5],
			dev.bufferAllocator(), maxVerts * sizeof(Vec4f),
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::vertexBuffer, devMem};

		// 2. init
		bufs_.lights.init(ibs[0]);
		bufs_.segments.init(ibs[1]);
		bufs_.materials.init(ibs[2]);
		bufs_.raysCmd.init(ibs[3]);
		bufs_.positions.init(ibs[4]);
		bufs_.colors.init(ibs[5]);

		vpp::fillDirect(cb, bufs_.lights, tkn::bytes(lights));
		vpp::fillDirect(cb, bufs_.segments, tkn::bytes(segs));
		vpp::fillDirect(cb, bufs_.materials, tkn::bytes(mats));

		vk::DrawIndirectCommand cmd {};
		cmd.instanceCount = 1;
		vpp::fillDirect(cb, bufs_.raysCmd, tkn::bytes(cmd));

		nLights_ = lights.size();
		light_ = *lights.begin();
	}

	void initCompute() {
		auto& dev = vulkanDevice();

		// layouts
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute),
		};

		comp_.dsLayout = {dev, bindings};
		comp_.pipeLayout = {dev, {{comp_.dsLayout.vkHandle()}}, {}};

		// pipe
		vpp::ShaderModule module(dev, rays_rays_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = comp_.pipeLayout;
		cpi.stage.module = module;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		comp_.pipe = {dev, cpi};

		// ubo
		auto uboSize = sizeof(Vec2f) * 2 + sizeof(float);
		auto hostBits = dev.hostMemoryTypes();
		comp_.ubo = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, hostBits};

		// ds
		comp_.ds = {dev.descriptorAllocator(), comp_.dsLayout};

		vpp::DescriptorSetUpdate dsu(comp_.ds);
		dsu.storage({{{bufs_.segments}}});
		dsu.storage({{{bufs_.materials}}});
		dsu.storage({{{bufs_.lights}}});
		dsu.storage({{{bufs_.raysCmd}}});
		dsu.storage({{{bufs_.positions}}});
		dsu.storage({{{bufs_.colors}}});
		dsu.uniform({{{comp_.ubo}}});
	}

	void initGfx() {
		auto& dev = vulkanDevice();

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
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
		};

		gfx_.dsLayout = {dev, bindings};
		gfx_.pipeLayout = {dev, {{gfx_.dsLayout.vkHandle()}}, {}};

		// pipe
		auto vert = vpp::ShaderModule(dev, rays_ray_vert_data);
		auto frag = vpp::ShaderModule(dev, rays_ray_frag_data);

		vpp::GraphicsPipelineInfo pipeInfo(gfx_.rp, gfx_.pipeLayout, {{{
				{vert, vk::ShaderStageBits::vertex},
				{frag, vk::ShaderStageBits::fragment}
		}}}, 0u, samples());

		vk::VertexInputBindingDescription bufferBindings[] = {
			{0, sizeof(Vec2f), vk::VertexInputRate::vertex},
			{1, sizeof(Vec4f), vk::VertexInputRate::vertex},
		};

		vk::VertexInputAttributeDescription attribs[2] = {};
		attribs[0].format = vk::Format::r32g32Sfloat;
		attribs[1].format = vk::Format::r32g32b32a32Sfloat;
		attribs[1].binding = 1;
		attribs[1].location = 1;

		pipeInfo.vertex.vertexBindingDescriptionCount = 2;
		pipeInfo.vertex.pVertexBindingDescriptions = bufferBindings;
		pipeInfo.vertex.vertexAttributeDescriptionCount = 2;
		pipeInfo.vertex.pVertexAttributeDescriptions = attribs;

		pipeInfo.assembly.topology = vk::PrimitiveTopology::lineList;
		pipeInfo.rasterization.lineWidth = 1.f;

		// additive color blending
		// we don't really care about alpha
		vk::PipelineColorBlendAttachmentState blenda;
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
		pipeInfo.blend.attachmentCount = 1;
		pipeInfo.blend.pAttachments = &blenda;

		gfx_.pipe = {dev, pipeInfo.info()};

		// ubo
		auto uboSize = sizeof(nytl::Mat4f);
		auto hostMem = dev.hostMemoryTypes();
		gfx_.ubo = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, hostMem};

		// ds
		gfx_.ds = {dev.descriptorAllocator(), gfx_.dsLayout};

		vpp::DescriptorSetUpdate dsu(gfx_.ds);
		dsu.uniform({{{gfx_.ubo}}});
	}

	void initBuffers(const vk::Extent2D& size,
			nytl::Span<RenderBuffer> bufs) override {
		App::initBuffers(size, bufs);

		auto& dev = vulkanDevice();
		auto info = vpp::ViewableImageCreateInfo(renderFormat,
			vk::ImageAspectBits::color, size,
			vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::transferDst |
			vk::ImageUsageBits::storage);
		dlg_assert(vpp::supported(dev, info.img));
		tss_.history = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		info.img.usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled;
		dlg_assert(vpp::supported(dev, info.img));
		gfx_.target = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};

		// update framebuffer
		auto attachments = {gfx_.target.vkImageView()};
		auto fbi = vk::FramebufferCreateInfo({}, gfx_.rp,
			attachments.size(), attachments.begin(),
			size.width, size.height, 1);
		gfx_.fb = {dev, fbi};

		// update descriptors
		vpp::DescriptorSetUpdate tu(tss_.ds);
		tu.imageSampler({{{}, gfx_.target.vkImageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		tu.storage({{{}, tss_.history.vkImageView(),
			vk::ImageLayout::general}});

		vpp::DescriptorSetUpdate pu(pp_.ds);
		pu.imageSampler({{{}, tss_.history.vkImageView(),
			vk::ImageLayout::general}});

		vpp::apply({{tu, pu}});

		// TODO: hack
		// should simply be added as dependency (via semaphore) to
		// the next render submission
		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		vk::ImageMemoryBarrier barrier;
		barrier.image = tss_.history.image();
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
		vk::cmdClearColorImage(cb, tss_.history.image(),
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

	// yeah, we could use BufferMemoryBarriers in this function
	// but i'm too lazy rn. And my gpu/mesa impl doesn't care anyways :(
	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);

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
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, comp_.pipe);
		tkn::cmdBindComputeDescriptors(cb, comp_.pipeLayout, 0, {comp_.ds});

		u32 dy = sampleCount / 16;
		vk::cmdDispatch(cb, nLights_, dy, 1);

		// make sure the generated rays are visible to gfx pipe
		barrier.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		barrier.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::vertexAttributeRead |
			vk::AccessBits::indirectCommandRead;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::allGraphics, {}, {{barrier}}, {}, {});

		// render into offscreen buffer
		auto [width, height] = swapchainInfo().imageExtent;
		vk::Viewport vp{0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		std::array<vk::ClearValue, 1u> cv {};
		cv[0] = {{0.f, 0.f, 0.f, 0.f}};
		vk::cmdBeginRenderPass(cb, {gfx_.rp, gfx_.fb,
			{0u, 0u, width, height},
			std::uint32_t(cv.size()), cv.data()}, {});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfx_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, gfx_.pipeLayout, 0, {gfx_.ds});
		vk::cmdBindVertexBuffers(cb, 0, {{
			bufs_.positions.buffer().vkHandle(),
			bufs_.colors.buffer().vkHandle(),
		}}, {{
			bufs_.positions.offset(),
			bufs_.colors.offset(),
		}});
		vk::cmdDrawIndirect(cb, bufs_.raysCmd.buffer(),
			bufs_.raysCmd.offset(), 1, 0);

		vk::cmdEndRenderPass(cb);

		// apply tss
		// work group size of tss.comp is 8x8
		auto cx = (width + 7) >> 3; // ceil(width / 8)
		auto cy = (height + 7) >> 3; // ceil(height / 8)
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, tss_.pipe);
		tkn::cmdBindComputeDescriptors(cb, tss_.pipeLayout, 0, {tss_.ds});
		vk::cmdDispatch(cb, cx, cy, 1);

		// make sure tss results are available for post processing
		vk::ImageMemoryBarrier ibarrier;
		ibarrier.image = tss_.history.image();
		ibarrier.oldLayout = vk::ImageLayout::general;
		ibarrier.newLayout = vk::ImageLayout::general;
		ibarrier.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		ibarrier.dstAccessMask = vk::AccessBits::shaderRead;
		ibarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{ibarrier}});
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, pp_.pipeLayout, 0, {pp_.ds});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad
	}

	void update(double dt) override {
		App::update(dt);

		if(next_) {
			// next_ = false;
			App::scheduleRedraw();
		}

		time_ += dt;
	}

	void updateDevice() override {
		App::updateDevice();

		auto map = gfx_.ubo.memoryMap();
		auto span = map.span();
		tkn::write(span, matrix_);

		map = comp_.ubo.memoryMap();
		span = map.span();
		tkn::write(span, view_.center - 0.5f * view_.size);
		tkn::write(span, view_.size);
		tkn::write(span, time_);
		map.flush();

		if(updateLight_) {
			updateLight_ = false;

			// TODO: hack
			auto& dev = vulkanDevice();
			auto& qs = dev.queueSubmitter();
			auto qfam = qs.queue().family();
			auto cb = dev.commandAllocator().get(qfam);
			vk::beginCommandBuffer(cb, {});
			vpp::fillDirect(cb, bufs_.lights, tkn::bytes(light_));
			vk::endCommandBuffer(cb);
			qs.wait(qs.add(cb));
		}
	}

	void updateMatrix() {
		auto wsize = window().size();
		view_.size = tkn::levelViewSize(wsize.x / float(wsize.y), 15.f);
		matrix_ = tkn::levelMatrix(view_);
		updateView_ = true;
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		updateMatrix();
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(ev.pressed && ev.keycode == ny::Keycode::n) {
			next_ = true;
			return true;
		}

		return false;
	}

	bool touchBegin(const ny::TouchBeginEvent& ev) override {
		if(App::touchBegin(ev)) {
			return true;
		}

		auto pos = tkn::windowToLevel(window().size(), view_, nytl::Vec2i(ev.pos));
		light_.pos = pos;
		updateLight_ = true;
		return true;
	}

	void touchUpdate(const ny::TouchUpdateEvent& ev) override {
		auto pos = tkn::windowToLevel(window().size(), view_, nytl::Vec2i(ev.pos));
		light_.pos = pos;
		updateLight_ = true;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		auto pos = tkn::windowToLevel(window().size(), view_, ev.position);
		light_.pos = pos;
		updateLight_ = true;
	}

	const char* name() const override { return "rays"; }
	bool needsDepth() const override { return false; }

protected:
	Light light_;
	tkn::LevelView view_ {};
	nytl::Mat4f matrix_;
	unsigned nLights_ {};
	float time_ {};
	bool next_ {true};

	bool updateView_ {true};
	bool updateLight_ {false};
	vpp::Sampler sampler_;

	struct {
		vpp::SubBuffer segments;
		vpp::SubBuffer lights;
		vpp::SubBuffer materials;
		vpp::SubBuffer raysCmd;
		vpp::SubBuffer positions;
		vpp::SubBuffer colors;
	} bufs_;

	struct {
		vpp::SubBuffer ubo;
		vpp::Pipeline pipe;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
	} comp_;

	struct {
		vpp::SubBuffer ubo;
		vpp::Pipeline pipe;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;

		vpp::RenderPass rp;
		vpp::Framebuffer fb;
		vpp::ViewableImage target;
	} gfx_;

	struct {
		vpp::ViewableImage history;
		vpp::Pipeline pipe;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
	} tss_;

	struct {
		vpp::Pipeline pipe;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
	} pp_;
};

int main(int argc, const char** argv) {
	RaysApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
