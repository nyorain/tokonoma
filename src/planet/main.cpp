#include <tkn/app.hpp>
#include <tkn/ccam.hpp>
#include <tkn/render.hpp>
#include <tkn/bits.hpp>
#include <tkn/util.hpp>
#include <tkn/texture.hpp>
#include <tkn/formats.hpp>
#include <tkn/scene/shape.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/debug.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/planet.model.vert.h>
#include <shaders/planet.model.frag.h>
#include <shaders/planet.update.comp.h>
#include <shaders/planet.dispatch.comp.h>
#include <shaders/planet.pp.frag.h>

using namespace tkn::types;

#define nameHandle(x) vpp::nameHandle(x, #x)

class PlanetApp : public tkn::App {
public:
	static constexpr auto colorFormat = vk::Format::r16g16b16a16Sfloat;

	struct UboData {
		nytl::Mat4f vp;
		nytl::Vec3f eye;
		u32 enable;
	};

public:
	using Base = tkn::App;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		cam_.useSpaceshipControl();
		auto& dev = vkDevice();
		depthFormat_ = tkn::findDepthFormat(dev);
		if(depthFormat_ == vk::Format::undefined) {
			dlg_error("No depth format supported");
			return false;
		}

		sampler_ = vpp::Sampler(dev, tkn::linearSamplerInfo());

		auto devMem = dev.deviceMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(UboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// indirect dispatch buffer
		comp_.dispatch = {dev.bufferAllocator(),
			sizeof(vk::DrawIndirectCommand) + sizeof(vk::DispatchIndirectCommand),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::indirectBuffer |
				vk::BufferUsageBits::transferDst, devMem};

		// vertices/indices
		auto shape = tkn::generateIco(0u);
		std::vector<nytl::Vec4f> vverts;
		for(auto& v : shape.positions) {
			vverts.emplace_back(v);
		}

		auto verts = tkn::bytes(vverts);
		auto inds = tkn::bytes(shape.indices);

		vertices_ = {dev.bufferAllocator(), verts.size(),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst, devMem};
		indices_ = {dev.bufferAllocator(), inds.size(),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst, devMem};

		// key buffers
		// TODO: allow buffers to grow dynamically
		// condition can simply be checked. Modify compute shaders
		// to check for out-of-bounds
		auto triCount = 10 * 1024 * 1024;
		auto bufSize = triCount * sizeof(nytl::Vec2u32);
		auto usage = vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::transferDst;

		keys0_ = {dev.bufferAllocator(), bufSize + 8, usage, devMem};
		keys1_ = {dev.bufferAllocator(), bufSize + 8, usage, devMem};

		// upload work
		// writing initial data to buffers
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		// write initial data to buffers
		tkn::WriteBuffer data0;

		dlg_assert(shape.indices.size() % 3 == 0);
		auto numTris = shape.indices.size() / 3;
		write(data0, u32(numTris)); // counter
		write(data0, 0.f); // padding

		for(auto i = 0u; i < numTris; ++i) {
			write(data0, nytl::Vec2u32 {1, i});
		}

		struct {
			vk::DrawIndirectCommand draw;
			vk::DispatchIndirectCommand dispatch;
		} cmds {};

		cmds.draw.firstInstance = 0;
		cmds.draw.firstVertex = 0;
		cmds.draw.instanceCount = numTris;
		cmds.draw.vertexCount = 3; // triangle (list)
		cmds.dispatch.x = numTris;
		cmds.dispatch.y = 1;
		cmds.dispatch.z = 1;

		auto stage0 = vpp::fillStaging(cb, keys0_, data0.buffer);
		auto stage1 = vpp::fillStaging(cb, comp_.dispatch, tkn::bytes(cmds));
		auto stage3 = vpp::fillStaging(cb, vertices_, verts);
		auto stage4 = vpp::fillStaging(cb, indices_, inds);

		// heightmap
		auto wb = tkn::WorkBatcher(dev);
		wb.cb = cb;
		auto img = tkn::loadImage("heightmap.ktx");
		auto initHeightmap = tkn::createTexture(wb, std::move(img));
		heightmap_ = tkn::initTexture(initHeightmap, wb);
		nameHandle(heightmap_);

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		initUpdateComp();
		initIndirectComp();
		initRenderTerrain();
		initRenderPP();

		return true;
	}

	void initUpdateComp() {
		auto& dev = vkDevice();
		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // read, old keys
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // write, new keys
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute), // camera
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // verts
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // inds
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_.vkHandle()), // heightmap
		};

		comp_.update.dsLayout.init(dev, bindings);
		nameHandle(comp_.update.dsLayout);
		comp_.update.pipeLayout = {dev, {{comp_.update.dsLayout.vkHandle()}}, {}};
		nameHandle(comp_.update.pipeLayout);

		comp_.update.ds = {dev.descriptorAllocator(), comp_.update.dsLayout};
		nameHandle(comp_.update.ds);

		vpp::DescriptorSetUpdate dsu(comp_.update.ds);
		dsu.storage({{{keys0_}}});
		dsu.storage({{{keys1_}}});
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{vertices_}}});
		dsu.storage({{{indices_}}});
		dsu.imageSampler({{{{}, heightmap_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});

		auto mod = vpp::ShaderModule(dev, planet_update_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = comp_.update.pipeLayout;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		comp_.update.pipe = {dev, cpi};
	}

	void initIndirectComp() {
		auto& dev = vkDevice();
		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // dispatch buf
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute), // counter new keys
		};

		comp_.indirect.dsLayout.init(dev, bindings);
		nameHandle(comp_.indirect.dsLayout);
		comp_.indirect.pipeLayout = {dev, {{comp_.indirect.dsLayout.vkHandle()}}, {}};
		nameHandle(comp_.indirect.pipeLayout);

		comp_.indirect.ds = {dev.descriptorAllocator(), comp_.indirect.dsLayout};
		nameHandle(comp_.indirect.ds);

		vpp::DescriptorSetUpdate dsu(comp_.indirect.ds);
		dsu.storage({{{comp_.dispatch}}});
		dsu.storage({{{keys1_}}});

		auto mod = vpp::ShaderModule(dev, planet_dispatch_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = comp_.indirect.pipeLayout;
		cpi.stage.module = mod;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;
		comp_.indirect.pipe = {dev, cpi};
	}

	void initRenderTerrain() {
		auto& dev = vkDevice();
		auto pass0 = {0u, 1u};
		auto rpi = tkn::renderPassInfo({{colorFormat, depthFormat_}}, {{pass0}});
		terrain_.rp = {dev, rpi.info()};
		nameHandle(terrain_.rp);

		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex), // scene data
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::vertex), // vertices
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::vertex), // indices
			// heightmap
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment,
				&sampler_.vkHandle()),
		};

		terrain_.dsLayout.init(dev, bindings);
		nameHandle(terrain_.dsLayout);
		terrain_.pipeLayout = {dev, {{terrain_.dsLayout}}, {}};
		nameHandle(terrain_.pipeLayout);

		terrain_.ds = {dev.descriptorAllocator(), terrain_.dsLayout};
		nameHandle(terrain_.ds);

		vpp::DescriptorSetUpdate dsu(terrain_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{vertices_}}});
		dsu.storage({{{indices_}}});
		dsu.imageSampler({{{{}, heightmap_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});

		auto vert = vpp::ShaderModule(dev, planet_model_vert_data);
		auto frag = vpp::ShaderModule(dev, planet_model_frag_data);
		auto gpi = vpp::GraphicsPipelineInfo(terrain_.rp, terrain_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}});

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		gpi.rasterization.polygonMode = vk::PolygonMode::fill;

		constexpr auto stride = sizeof(nytl::Vec2u32); // uvec2
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::instance};

		vk::VertexInputAttributeDescription attributes[1];
		attributes[0].format = vk::Format::r32g32Uint;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 1u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;

		terrain_.pipe = vpp::Pipeline(vkDevice(), gpi.info());
	}

	void initRenderPP() {
		auto& dev = vkDevice();
		auto pass0 = {0u};
		auto rpi = tkn::renderPassInfo({{swapchainInfo().imageFormat}}, {{pass0}});
		rpi.attachments[0].finalLayout = vk::ImageLayout::presentSrcKHR;
		pp_.rp = {dev, rpi.info()};
		nameHandle(pp_.rp);

		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_.vkHandle())
		};

		pp_.dsLayout.init(dev, bindings);
		nameHandle(pp_.dsLayout);
		pp_.pipeLayout = {dev, {{pp_.dsLayout}}, {}};
		nameHandle(pp_.pipeLayout);
		pp_.ds = {dev.descriptorAllocator(), pp_.dsLayout};
		nameHandle(pp_.ds);

		auto vert = vpp::ShaderModule(dev, tkn_fullscreen_vert_data);
		auto frag = vpp::ShaderModule(dev, planet_pp_frag_data);
		auto gpi = vpp::GraphicsPipelineInfo(pp_.rp, pp_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}});

		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		pp_.pipe = vpp::Pipeline(dev, gpi.info());
	}

	void compute(vk::CommandBuffer cb) {
		// reset counter in dst buffer to 0
		vk::cmdFillBuffer(cb, keys1_.buffer(), keys1_.offset(), 4, 0);

		// make sure the reset is visible
		vk::BufferMemoryBarrier barrier1;
		barrier1.buffer = keys1_.buffer();
		barrier1.offset = keys1_.offset();
		barrier1.size = 4u;
		barrier1.srcAccessMask = vk::AccessBits::transferWrite;
		barrier1.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;

		// run update pipeline
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader, {}, {}, {{barrier1}}, {});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, comp_.update.pipe);
		tkn::cmdBindComputeDescriptors(cb, comp_.update.pipeLayout, 0, {comp_.update.ds});
		vk::cmdDispatchIndirect(cb, comp_.dispatch.buffer(),
			comp_.dispatch.offset() + sizeof(vk::DrawIndirectCommand));

		// make sure updates in keys1_ is visible
		vk::BufferMemoryBarrier barrier0;
		barrier0.buffer = keys0_.buffer();
		barrier0.offset = keys0_.offset();
		barrier0.size = keys0_.size();
		barrier0.srcAccessMask = vk::AccessBits::shaderRead;
		barrier0.dstAccessMask = vk::AccessBits::transferWrite;

		barrier1.size = keys1_.size();
		barrier1.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		barrier1.dstAccessMask = vk::AccessBits::transferRead |
			vk::AccessBits::shaderRead;

		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer |
				vk::PipelineStageBits::vertexInput |
				vk::PipelineStageBits::computeShader,
			{}, {}, {{barrier0, barrier1}}, {});

		// run indirect pipe to build commands
		// We can't do this with a simple copy since for the dispatch size
		// we have to divide by the compute gropu size. And running
		// a compute shader is likely to be faster then reading the
		// counter value to cpu, computing the division, and writing
		// it back to the gpu.
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, comp_.indirect.pipe);
		tkn::cmdBindComputeDescriptors(cb, comp_.indirect.pipeLayout, 0,
			{comp_.indirect.ds});
		vk::cmdDispatch(cb, 1, 1, 1);

		// copy from keys1_ (the new triangles) to keys0_ (which are
		// used for drawing and in the next update step).
		// we could alternatively use ping-ponging and do 2 steps per
		// frame or just use 2 completely independent command buffers.
		// May be more efficient but harder to sync.
		auto keys1 = vpp::BufferSpan(keys1_.buffer(), keys1_.size(),
			keys1_.offset());
		tkn::cmdCopyBuffer(cb, keys1, keys0_);

		barrier0.srcAccessMask = vk::AccessBits::transferWrite;
		barrier0.dstAccessMask = vk::AccessBits::shaderRead;

		barrier1.srcAccessMask = vk::AccessBits::transferRead;
		barrier1.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::BufferMemoryBarrier barrierIndirect;
		barrierIndirect.buffer = comp_.dispatch.buffer();
		barrierIndirect.offset = comp_.dispatch.offset();
		barrierIndirect.size = comp_.dispatch.size();
		barrierIndirect.srcAccessMask = vk::AccessBits::shaderWrite;
		barrierIndirect.dstAccessMask = vk::AccessBits::indirectCommandRead;

		// make sure the copy is visible for drawing (and the next step)
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer |
				vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::vertexShader |
				vk::PipelineStageBits::drawIndirect |
				vk::PipelineStageBits::computeShader,
			{}, {}, {{barrier0, barrier1, barrierIndirect}}, {});
	}

	void renderTerrain(vk::CommandBuffer cb) {
		tkn::cmdBindGraphicsDescriptors(cb, terrain_.pipeLayout, 0, {terrain_.ds});
		vk::cmdBindVertexBuffers(cb, 0, {{keys0_.buffer().vkHandle()}},
			{{keys0_.offset() + 8}}); // skip counter and padding

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, terrain_.pipe);
		vk::cmdDrawIndirect(cb, comp_.dispatch.buffer(), comp_.dispatch.offset(), 1, 0);
	}

	void record(const RenderBuffer& rb) override {
		const auto [width, height] = swapchainInfo().imageExtent;
		auto cb = rb.commandBuffer;
		vk::beginCommandBuffer(cb, {});

		compute(cb);

		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		// render terrain
		auto cvT = std::array {
			vk::ClearValue{0.f, 0.f, 0.f, 1.f}, // color
			vk::ClearValue{1.f, 0u}, // depth
		};
		vk::cmdBeginRenderPass(cb, {
			terrain_.rp, terrain_.fb,
			{0u, 0u, width, height},
			u32(cvT.size()), cvT.data()
		}, {});
		renderTerrain(cb);
		vk::cmdEndRenderPass(cb);

		// post process
		auto cvpp = std::array {
			vk::ClearValue{0.f, 0.f, 0.f, 1.f}, // color
		};
		vk::cmdBeginRenderPass(cb, {
			pp_.rp, rb.framebuffer,
			{0u, 0u, width, height},
			u32(cvpp.size()), cvpp.data()
		}, {});
		tkn::cmdBindGraphicsDescriptors(cb, pp_.pipeLayout, 0, {pp_.ds});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pp_.pipe);
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad
		vk::cmdEndRenderPass(cb);

		vk::endCommandBuffer(rb.commandBuffer);
	}

	void initBuffers(const vk::Extent2D& size, nytl::Span<RenderBuffer> buffers) override {
		auto& dev = vkDevice();
		auto devMem = dev.deviceMemoryTypes();

		// depth
		auto usage = vk::ImageUsageBits::depthStencilAttachment |
			vk::ImageUsageBits::sampled;
		auto info = vpp::ViewableImageCreateInfo(depthFormat_,
			vk::ImageAspectBits::depth, size, usage);
		terrain_.depth = {dev.devMemAllocator(), info, devMem};

		// color
		usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::sampled;
		info = vpp::ViewableImageCreateInfo(colorFormat,
			vk::ImageAspectBits::color, size, usage);
		terrain_.color = {dev.devMemAllocator(), info, devMem};

		// fb
		auto attachments = {terrain_.color.vkImageView(), terrain_.depth.vkImageView()};
		vk::FramebufferCreateInfo fbi;
		fbi.renderPass = terrain_.rp;
		fbi.width = size.width;
		fbi.height = size.height;
		fbi.layers = 1;
		fbi.attachmentCount = attachments.size();
		fbi.pAttachments = attachments.begin();
		terrain_.fb = {dev, fbi};

		// update pp ds
		vpp::DescriptorSetUpdate dsu(pp_.ds);
		dsu.imageSampler({{{}, terrain_.color.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.apply();

		// render buffers for pp
		fbi.renderPass = pp_.rp;
		for(auto& buf : buffers) {
			auto attachments = {buf.imageView.vkHandle()};
			fbi.attachmentCount = attachments.size();
			fbi.pAttachments = attachments.begin();
			buf.framebuffer = {dev, fbi};
		}
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();
		cam_.update(swaDisplay(), dt);
	}

	void updateDevice() override {
		Base::updateDevice();

		if(cam_.needsUpdate) {
			cam_.needsUpdate = false;

			auto map = ubo_.memoryMap();
			auto span = map.span();
			auto VP = cam_.viewProjectionMatrix();

			tkn::write(span, VP);
			tkn::write(span, cam_.position());
			tkn::write(span, u32(!frozen_));
			map.flush();
		}
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		cam_.aspect({w, h});
	}

	const char* name() const override { return "planet"; }

private:
	tkn::ControlledCamera cam_;
	vpp::ViewableImage heightmap_;
	vpp::Sampler sampler_;
	vk::Format depthFormat_;

	vpp::SubBuffer vertices_;
	vpp::SubBuffer indices_;

	vpp::SubBuffer ubo_;
	vpp::SubBuffer keys0_;
	vpp::SubBuffer keys1_; // temporary buffer we write updates to

	bool frozen_ {false};

	// render pass: render terrain
	struct {
		vpp::RenderPass rp;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;

		vpp::ViewableImage depth;
		vpp::ViewableImage color;
		vpp::Framebuffer fb;
	} terrain_;

	// render pass: post-process/tonemap
	struct {
		vpp::RenderPass rp;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
	} pp_;

	struct {
		struct {
			vpp::PipelineLayout pipeLayout;
			vpp::Pipeline pipe;
			vpp::TrDsLayout dsLayout;
			vpp::TrDs ds;
		} update;

		struct {
			vpp::PipelineLayout pipeLayout;
			vpp::Pipeline pipe;
			vpp::TrDsLayout dsLayout;
			vpp::TrDs ds;
		} indirect;

		vpp::SubBuffer dispatch; // indirect dispatch command
	} comp_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<PlanetApp>(argc, argv);
}

