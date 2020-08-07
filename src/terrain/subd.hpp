#pragma once

#include <tkn/types.hpp>
#include <tkn/pipeline.hpp>
#include <tkn/render.hpp>
#include <tkn/bits.hpp>
#include <tkn/util.hpp>

#include <vpp/debug.hpp>
#include <vpp/image.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>

using namespace tkn::types;

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

		// indirect write pipe
		auto infoProvider = tkn::ComputePipeInfoProvider::create(
			tkn::SpecializationInfo::create(u32(updateWorkGroupSize)));
		indirectWritePipe_ = {dev, "terrain/writeIndirect.comp", fswatch,
			{}, std::move(infoProvider)};

		auto& dsu = indirectWritePipe_.dsu();
		dsu(dispatchBuf_);
		dsu(keys1_);

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
		return indirectWritePipe_.updateDevice();
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
	tkn::ManagedComputePipe indirectWritePipe_;

	vpp::SubBuffer dispatchBuf_;
	vpp::SubBuffer keys0_;
	vpp::SubBuffer keys1_; // temporary update buffer
	std::size_t indexCount_;
};
