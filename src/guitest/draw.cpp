#include "context.hpp"
#include "draw.hpp"
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>

namespace rvg2 {

// DrawPool
DrawPool::DrawPool(Context& ctx, DevMemBits memBits) :
		indirectCmdBuf(ctx, vk::BufferUsageBits::indirectBuffer, memBits),
		bindingsCmdBuf(ctx, vk::BufferUsageBits::storageBuffer, memBits) {
}

// free util
bool operator==(const DrawState& a, const DrawState& b) {
	return
		a.transformBuffer == b.transformBuffer &&
		a.clipBuffer == b.clipBuffer &&
		a.paintBuffer == b.paintBuffer &&
		a.vertexBuffer == b.vertexBuffer &&
		a.indexBuffer == b.indexBuffer &&
		a.fontAtlas == b.fontAtlas &&
		std::equal(
			a.textures.begin(), a.textures.end(),
			b.textures.begin(), b.textures.end());
}

DrawCall::DrawCall(DrawPool& pool) : pool_(&pool) {
}

DrawCall::~DrawCall() {
	if(pool_ && reserved_) {
		pool_->indirectCmdBuf.free(indirectBufID_, reserved_);
		pool_->bindingsCmdBuf.free(bindingsBufID_, reserved_);
	}
}

void DrawCall::record(vk::CommandBuffer cb) {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		context().pipeLayout(), 0, {{ds_}}, {});
	vk::cmdBindVertexBuffers(cb, 0,
		{{state_.vertexBuffer.buffer()}}, {{state_.vertexBuffer.offset()}});
	vk::cmdBindIndexBuffer(cb, state_.indexBuffer.buffer(),
		state_.indexBuffer.offset(), vk::IndexType::uint32);

	const auto cmdSize = sizeof(vk::DrawIndexedIndirectCommand);
	const auto& indirectCmdBuf = pool_->indirectCmdBuf.buffer();
	auto offset = indirectCmdBuf.offset() + indirectBufID_ * cmdSize;

	if(context().multidrawIndirect()) {
		vk::cmdDrawIndexedIndirect(cb, indirectCmdBuf.buffer(), offset, size_, cmdSize);
	} else {
		for(auto i = 0u; i < size_; ++i) {
			// pass the draw command id per push constant
			const auto pcrValue = u32(bindingsBufID_);
			const auto pcrStages =
				vk::ShaderStageBits::fragment |
				vk::ShaderStageBits::vertex;

			vk::cmdPushConstants(cb, context().pipeLayout(), pcrStages, 0, 4u, &pcrValue);
			vk::cmdDrawIndexedIndirect(cb, indirectCmdBuf.buffer(), offset, 1, cmdSize);
			offset += cmdSize;
		}
	}
}

void DrawCall::reserve(unsigned i) {
	if(reserved_ >= i) {
		return;
	}

	auto nindirect = pool_->indirectCmdBuf.realloc(indirectBufID_, reserved_, i);
	auto nbindings = pool_->bindingsCmdBuf.realloc(bindingsBufID_, reserved_, i);

	rerecord_ |= ((nindirect != indirectBufID_) || (bindingsBufID_ != nbindings));

	indirectBufID_ = nindirect;
	bindingsBufID_ = nbindings;
	reserved_ = i;
}

void DrawCall::clear() {
	size_ = 0u;
}

unsigned DrawCall::add(nytl::Span<const Draw> draw) {
	// check that we enough space
	auto req = size_ + draw.size();
	if(req > reserved_) {
		reserve(2 * req); // overallocate
	}

	auto id = size_;
	write(id, draw);
	size_ += draw.size();

	// TODO; we could avoid this if we always make reserrved_
	// draws (with the not used ones empty).
	rerecord_ = true;
	return id;
}

unsigned DrawCall::add(const Draw& draw) {
	return add(nytl::Span<const Draw>(&draw, 1));
}

void DrawCall::write(unsigned id, nytl::Span<const Draw> draw) {
	dlg_assert(id + draw.size() <= size_);
	auto indirect = pool_->indirectCmdBuf.writable(indirectBufID_ + id, draw.size());
	auto bindings = pool_->bindingsCmdBuf.writable(bindingsBufID_ + id, draw.size());

	for(auto i = 0u; i < draw.size(); ++i) {
		indirect[i].firstIndex = draw[i].indexStart;
		indirect[i].indexCount = draw[i].indexCount;
		indirect[i].vertexOffset = draw[i].vertexOffset;
		indirect[i].firstInstance = 0u;
		indirect[i].instanceCount = 1u;

		bindings[i].clipCount = draw[i].clipCount;
		bindings[i].clipStart = draw[i].clipStart;
		bindings[i].transform = draw[i].transform;
		bindings[i].paint = draw[i].paint;
		bindings[i].type = draw[i].type;
		bindings[i].uvFadeWidth = draw[i].uvFadeWidth;
	}
}

void DrawCall::write(unsigned id, const Draw& draw) {
	write(id, nytl::Span<const Draw>(&draw, 1));
}

Draw DrawCall::get(unsigned id) {
	dlg_assert(id < size_);

	auto indirect = pool_->indirectCmdBuf.read(indirectBufID_ + id);
	auto bindings = pool_->bindingsCmdBuf.read(bindingsBufID_ + id);

	Draw draw;
	draw.vertexOffset = indirect.vertexOffset;
	draw.indexCount = indirect.indexCount;
	draw.indexStart = indirect.firstIndex;

	draw.clipStart = bindings.clipStart;
	draw.clipCount = bindings.clipCount;
	draw.transform = bindings.transform;
	draw.paint = bindings.paint;
	draw.type = bindings.type;
	draw.uvFadeWidth = bindings.uvFadeWidth;

	return draw;
}

void DrawCall::remove(unsigned id) {
	dlg_assert(id < size_);

	// yep, we potentially write a lot here, also have to update a lot of
	// gpu data.. But this should not happen often, and each command
	// is quite small afterall.
	// NOTE: we could instead just set indexCount to zero. That is probably
	// not better though since draw calls aren't just allocated but
	// potentially order-dependent.
	auto indirect = pool_->indirectCmdBuf.writable(indirectBufID_ + id, size_ - id);
	auto bindings = pool_->bindingsCmdBuf.writable(bindingsBufID_ + id, size_ - id);

	std::copy(indirect.begin(), indirect.end() - 1, indirect.begin() + 1);
	std::copy(bindings.begin(), bindings.end() - 1, bindings.begin() + 1);
	--size_;
}

// TODO
// DrawRecorder
DrawRecorder::DrawRecorder(Context& ctx, std::vector<DrawCall>& drawCalls) {
}

DrawRecorder::~DrawRecorder() {
}

// DrawDescriptor
DrawDescriptor::DrawDescriptor(Context& ctx) : context_(&ctx) {
	ds_ = {ctx.dsAllocator(), ctx.dsLayout()};
}

bool DrawDescriptor::updateDevice() {
	if(!updateDs_) {
		return false;
	}

	vpp::DescriptorSetUpdate dsu(ds_);

	// TODO: bind dummy buffer as fallback?
	// or warn about them here
	dsu.storage(state_.clipBuffer);
	dsu.storage(state_.transformBuffer);
	dsu.storage(state_.paintBuffer);

	auto& texs = state_.textures;
	auto binding = dsu.currentBinding();
	dlg_assert(texs.size() <= context().numBindableTextures());
	auto sampler = context().sampler();
	for(auto i = 0u; i < texs.size(); ++i) {
		dsu.imageSampler(texs[i], sampler, binding, i);
	}

	auto dummyImage = context().dummyImageView();
	for(auto i = texs.size(); i < context().numBindableTextures(); ++i) {
		dsu.imageSampler(dummyImage, sampler, binding, i);
	}

	dsu.storage(pool_->bindingsCmdBuf.buffer());
	dsu.imageSampler(state_.fontAtlas);

	// when descriptor indexing is active, no rerecord is needed
	return !context().descriptorIndexing();
}

} // namespace rvg2
