#include <tkn/singlePassApp.hpp>
#include <tkn/types.hpp>
#include <tkn/pipeline.hpp>
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
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>

#include <swa/swa.h>

using namespace tkn::types;

// TODO: move to vpp
template<typename T>
T& as(const vpp::MemoryMapView& view) {
	dlg_assert(view.size() >= sizeof(T));
	return *reinterpret_cast<T*>(view.ptr());
}

class Subdivider {
public:
	static constexpr auto maxTriCount = 10 * 1024 * 1024; // 10MB

	// The workgroup size that must be used by the update pipeline.
	static constexpr auto updateWorkGroupSize = 64u;

	struct InitData {
		vpp::SubBuffer stage0;
		vpp::SubBuffer stage1;
	};

public:
	Subdivider() = default;
	Subdivider(InitData& data, const vpp::Device& dev, tkn::FileWatcher& fswatch,
			std::size_t indexCount, vk::CommandBuffer initCb) {
		indexCount_ = indexCount;

		// indirect write pipe
		auto infoHandler = [](vk::ComputePipelineCreateInfo& cpi) {
			static tkn::ComputeGroupSizeSpec spec(updateWorkGroupSize);
			cpi.stage.pSpecializationInfo = &spec.spec;
			dlg_info("compHandler0: {}", *(unsigned*) cpi.stage.pSpecializationInfo->pData);
		};

		indirectWritePipe_ = {dev, "terrain/writeIndirect.comp", fswatch,
			{}, tkn::makeUniqueCallable(infoHandler)};

		// buffers
		auto bufSize = maxTriCount * sizeof(nytl::Vec2u32);
		auto usage = vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::transferDst;

		keys0_ = {dev.bufferAllocator(), bufSize + 8, usage, dev.deviceMemoryTypes()};
		keys1_ = {dev.bufferAllocator(), bufSize + 8, usage, dev.deviceMemoryTypes()};
		dispatchBuf_ = {dev.bufferAllocator(),
			sizeof(vk::DrawIndirectCommand) + sizeof(vk::DispatchIndirectCommand),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::indirectBuffer |
				vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};

		// write initial data to buffers
		tkn::WriteBuffer data0;

		dlg_assert(indexCount % 3 == 0);
		auto numTris = indexCount / 3;
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
		cmds.dispatch.x = tkn::ceilDivide(numTris, updateWorkGroupSize);;
		cmds.dispatch.y = 1;
		cmds.dispatch.z = 1;

		data.stage0 = vpp::fillStaging(initCb, keys0_, data0.buffer);
		data.stage1 = vpp::fillStaging(initCb, dispatchBuf_, tkn::bytes(cmds));
	}

	void resetCounter(vk::CommandBuffer cb) {
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

		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader, {}, {},
			{{barrier1}}, {});
	}

	// Expects the update pipeline to be bound
	void computeUpdate(vk::CommandBuffer cb) {
		vk::cmdDispatchIndirect(cb, dispatchBuf_.buffer(),
			dispatchBuf_.offset() + sizeof(vk::DrawIndirectCommand));

		// make sure updates in keys1_ is visible
		vk::BufferMemoryBarrier barrier0;
		barrier0.buffer = keys0_.buffer();
		barrier0.offset = keys0_.offset();
		barrier0.size = keys0_.size();
		barrier0.srcAccessMask = vk::AccessBits::shaderRead;
		barrier0.dstAccessMask = vk::AccessBits::transferWrite;

		vk::BufferMemoryBarrier barrier1;
		barrier1.buffer = keys1_.buffer();
		barrier1.offset = keys1_.offset();
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
	}

	void writeDispatch(vk::CommandBuffer cb) {
		// run indirect pipe to build commands
		// We can't do this with a simple copy since for the dispatch size
		// we have to divide by the compute gropu size. And running
		// a compute shader is likely to be faster then reading the
		// counter value to cpu, computing the division, and writing
		// it back to the gpu.
		tkn::cmdBind(cb, indirectWritePipe_);
		vk::cmdDispatch(cb, 1, 1, 1);

		// copy from keys1_ (the new triangles) to keys0_ (which are
		// used for drawing and in the next update step).
		// we could alternatively use ping-ponging and do 2 steps per
		// frame or just use 2 completely independent command buffers.
		// May be more efficient but harder to sync.
		tkn::cmdCopyBuffer(cb, keys1_, keys0_);

		vk::BufferMemoryBarrier barrier0;
		barrier0.buffer = keys0_.buffer();
		barrier0.offset = keys0_.offset();
		barrier0.size = keys0_.size();
		barrier0.srcAccessMask = vk::AccessBits::transferWrite;
		barrier0.dstAccessMask = vk::AccessBits::shaderRead;

		vk::BufferMemoryBarrier barrier1;
		barrier1.buffer = keys1_.buffer();
		barrier1.offset = keys1_.offset();
		barrier1.size = keys1_.size();
		barrier1.srcAccessMask = vk::AccessBits::transferRead;
		barrier1.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::BufferMemoryBarrier barrierIndirect;
		barrierIndirect.buffer = dispatchBuf_.buffer();
		barrierIndirect.offset = dispatchBuf_.offset();
		barrierIndirect.size = dispatchBuf_.size();
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

	// Expects the drawing graphics pipeline to be bound
	void draw(vk::CommandBuffer cb) {
		vk::cmdBindVertexBuffers(cb, 0, {{keys0_.buffer().vkHandle()}},
			{{keys0_.offset() + 8}}); // skip counter and padding
		vk::cmdDrawIndirect(cb, dispatchBuf_.buffer(), dispatchBuf_.offset(), 1, 0);
	}

	void update() {
		indirectWritePipe_.update();
	}

	bool updateDevice() {
		if(indirectWritePipe_.updateDevice()) {
			// TODO: only do this the first time
			vpp::DescriptorSetUpdate dsu(indirectWritePipe_.pipeState().dss[0]);
			dsu.storage({{{dispatchBuf_}}});
			dsu.storage({{{keys1_}}});
			return true;
		}

		return false;
	}

	bool loaded() const {
		return indirectWritePipe_.pipe();
	}

	vpp::BufferSpan keys0() const {
		return keys0_;
	}

	vpp::BufferSpan keys1() const {
		return keys1_;
	}

	static vk::VertexInputAttributeDescription vertexAttribute() {
		return {0, 0, vk::Format::r32g32Uint, 0};
	}

	static vk::VertexInputBindingDescription vertexBinding() {
		return {0, sizeof(nytl::Vec2u32), vk::VertexInputRate::instance};
	}

	static vk::PipelineVertexInputStateCreateInfo vertexInfo() {
		static auto attrib = vertexAttribute();
		static auto binding = vertexBinding();
		return {{},
			1, &binding,
			1, &attrib
		};
	}

protected:
	tkn::ComputePipelineState indirectWritePipe_;

	vpp::SubBuffer dispatchBuf_;
	vpp::SubBuffer keys0_;
	vpp::SubBuffer keys1_; // temporary update buffer
	std::size_t indexCount_;
};

class TerrainApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	struct UboData {
		nytl::Mat4f vp;
		nytl::Vec3f viewPos;
	};

public:
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto& dev = vkDevice();

		// graphics pipeline
		auto gfxHandler = [rp = renderPass()](vpp::GraphicsPipelineInfo& gpi) {
			gpi.renderPass(rp);
			gpi.depthStencil.depthTestEnable = true;
			gpi.depthStencil.depthWriteEnable = true;
			gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
			// gpi.rasterization.cullMode = vk::CullModeBits::back;
			gpi.rasterization.cullMode = vk::CullModeBits::none;
			gpi.rasterization.polygonMode = vk::PolygonMode::fill;
			gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
			gpi.vertex = Subdivider::vertexInfo();
		};

		renderPipe_ = {dev, {"terrain/terrain.vert"}, {"terrain/terrain.frag"},
			fileWatcher_, tkn::makeUniqueCallable(gfxHandler)};

		// compute pipeline
		auto compHandler = [](vk::ComputePipelineCreateInfo& cpi) {
			static tkn::ComputeGroupSizeSpec spec(Subdivider::updateWorkGroupSize);
			cpi.stage.pSpecializationInfo = &spec.spec;
			dlg_info("compHandler0: {}", *(unsigned*) cpi.stage.pSpecializationInfo->pData);
		};

		updatePipe_ = {dev, "terrain/update.comp", fileWatcher_, {},
			tkn::makeUniqueCallable(compHandler)};

		// data
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

		// upload data
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		Subdivider::InitData initSubd;
		subd_ = {initSubd, dev, fileWatcher_, shape.indices.size(), cb};

		auto stage3 = vpp::fillStaging(cb, vertices_, verts);
		auto stage4 = vpp::fillStaging(cb, indices_, inds);

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		return true;
	}

	void update(double dt) override {
		Base::update(dt);

		cam_.update(swaDisplay(), dt);

		fileWatcher_.update();
		renderPipe_.update();
		updatePipe_.update();
		subd_.update();

		Base::scheduleRedraw();
	}

	void updateDevice() override {
		if(cam_.needsUpdate) {
			cam_.needsUpdate = false;

			auto& data = as<UboData>(uboMap_);
			data.vp = cam_.viewProjectionMatrix();
			data.viewPos = cam_.position();

			uboMap_.flush();
		}

		if(renderPipe_.updateDevice() ||
				updatePipe_.updateDevice() ||
				subd_.updateDevice()) {

			if(pipesLoaded()) {
				dlg_info("update dss");
				Base::scheduleRerecord();

				// TODO: only do this the first time.
				// Integrate mechanism for that into ReloadablePipeline or
				// xPipelineState. Either a general callback or/and
				// something highlevel specifically for descriptor updates.
				vpp::DescriptorSetUpdate dsuRender(renderPipe_.pipeState().dss[0]);
				dsuRender.uniform({{{ubo_}}});
				dsuRender.storage({{{vertices_}}});
				dsuRender.storage({{{indices_}}});

				vpp::DescriptorSetUpdate dsuUpdate(updatePipe_.pipeState().dss[0]);
				dsuUpdate.uniform({{{ubo_}}});
				dsuUpdate.storage({{{vertices_}}});
				dsuUpdate.storage({{{indices_}}});
				dsuUpdate.storage({{{subd_.keys0()}}});
				dsuUpdate.storage({{{subd_.keys1()}}});

				vpp::apply({{dsuRender, dsuUpdate}});
			}
		}
	}

	void beforeRender(vk::CommandBuffer cb) override {
		if(!pipesLoaded()) {
			return;
		}

		subd_.resetCounter(cb);
		tkn::cmdBind(cb, updatePipe_);
		subd_.computeUpdate(cb);
		subd_.writeDispatch(cb);
	}

	void render(vk::CommandBuffer cb) override {
		if(!pipesLoaded()) {
			return;
		}

		tkn::cmdBind(cb, renderPipe_);
		subd_.draw(cb);
	}

	bool pipesLoaded() const {
		return bool(renderPipe_.pipe()) &&
			bool(updatePipe_.pipe()) &&
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

	const char* name() const override { return "terrain"; }
	bool needsDepth() const override { return true; }

protected:
	tkn::GraphicsPipelineState renderPipe_;
	tkn::ComputePipelineState updatePipe_;

	tkn::ControlledCamera cam_;
	tkn::FileWatcher fileWatcher_;

	vpp::SubBuffer ubo_;
	vpp::SubBuffer vertices_;
	vpp::SubBuffer indices_;

	vpp::MemoryMapView uboMap_;
	// vpp::ViewableImage heightmap_; // TODO

	Subdivider subd_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<TerrainApp>(argc, argv);
}
