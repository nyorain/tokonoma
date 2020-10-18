#include "context.hpp"
#include "draw.hpp"
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>

namespace rvg2 {

const BufferRef::Allocation& BufferRef::fullAllocation() {
	static Allocation ret {0, vk::wholeSize};
	return ret;
}

BufferRef::BufferRef(const vpp::SubBuffer& sb) :
		ref(Sub{&sb.bufferPtr(), &sb.allocation()}) {
}

BufferRef::BufferRef(vpp::BufferSpan span) : ref(span) {
}

BufferRef::BufferRef(const vk::Buffer& buf, const Allocation& alloc) :
		ref(Raw{&buf, &alloc}) {
}

vk::DescriptorBufferInfo BufferRef::info() const {
	return std::visit(Visitor{
		[](vpp::BufferSpan span) ->vk::DescriptorBufferInfo {
			return {span.buffer(), span.offset(), span.size()};
		}, [](Raw raw) -> vk::DescriptorBufferInfo {
			if(!raw.buffer || !raw.allocation) {
				return {};
			}
			return {*raw.buffer, raw.allocation->offset, raw.allocation->size};
		}, [](Sub sub) -> vk::DescriptorBufferInfo {
			if(!sub.buffer || !sub.allocation) {
				return {};
			}
			return {(*sub.buffer)->vkHandle(), sub.allocation->offset, sub.allocation->size};
		}}, ref);
}

bool BufferRef::valid() const {
	return std::visit(Visitor{
		[](vpp::BufferSpan span) { return span.valid(); },
		[](Raw raw) { return raw.buffer && raw.allocation; },
		[](Sub sub) { return sub.buffer && sub.allocation; }
	}, ref);
}

bool BufferRef::nonempty() const {
	return std::visit(Visitor{
		[](vpp::BufferSpan span) { return span.valid(); },
		[](Raw raw) { return raw.buffer && raw.allocation && raw.allocation->size; },
		[](Sub sub) { return sub.buffer && sub.allocation && sub.allocation->size; }
	}, ref);
}

bool same(const vk::DescriptorBufferInfo& a, const vk::DescriptorBufferInfo& b) {
	return a.buffer == b.buffer &&
		((!a.buffer && !b.buffer) || (a.offset == b.offset && a.range == b.range));
}

bool operator==(const BufferRef& a, const BufferRef& b) {
	if(a.ref.index() != b.ref.index()) {
		return false;
	}

	if(auto sa = std::get_if<vpp::BufferSpan>(&a.ref)) {
		auto sb = std::get_if<vpp::BufferSpan>(&b.ref);
		return *sa == *sb;
	} else if(auto sa = std::get_if<BufferRef::Raw>(&a.ref)) {
		auto sb = std::get_if<BufferRef::Raw>(&b.ref);
		return sa->buffer == sb->buffer && sa->allocation == sb->allocation;
	} else if(auto sa = std::get_if<BufferRef::Sub>(&a.ref)) {
		auto sb = std::get_if<BufferRef::Sub>(&b.ref);
		return sa->buffer == sb->buffer && sa->allocation == sb->allocation;
	}

	return false;
}

// DrawPool
DrawPool::DrawPool(UpdateContext& ctx, DevMemBits memBits) {
	init(ctx, memBits);
}

void DrawPool::init(UpdateContext& ctx, DevMemBits memBits) {
	indirectCmdBuf.init(ctx, vk::BufferUsageBits::indirectBuffer, memBits);
	bindingsCmdBuf.init(ctx, vk::BufferUsageBits::storageBuffer, memBits);
}

// free util
bool operator==(const DrawState& a, const DrawState& b) {
	return
		a.transformBuffer == b.transformBuffer &&
		a.clipBuffer == b.clipBuffer &&
		a.paintBuffer == b.paintBuffer &&
		a.drawBuffer == b.drawBuffer &&
		a.fontAtlas == b.fontAtlas &&
		std::equal(
			a.textures.begin(), a.textures.end(),
			b.textures.begin(), b.textures.end());
}

bool valid(const DrawState& state) {
	return state.drawBuffer.valid() &&
		state.clipBuffer.valid() &&
		state.transformBuffer.valid() &&
		state.paintBuffer.valid() &&
		state.fontAtlas;
}

// DrawCall
DrawCall::DrawCall(DrawPool& pool) : pool_(&pool) {
}

DrawCall::~DrawCall() {
	if(pool_ && reserved_) {
		pool_->indirectCmdBuf.free(indirectBufID_, reserved_);
		pool_->bindingsCmdBuf.free(bindingsBufID_, reserved_);
	}
}

void DrawCall::record(vk::CommandBuffer cb, bool bindPipe, bool bindVertsInds) const {
	dlg_assert(pool_);
	dlg_assert(ds_);
	dlg_assert(indexBuffer_.valid());
	dlg_assert(vertexBuffer_.valid());


	if(bindPipe) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, context().pipe());
	}

	if(bindVertsInds) {
		auto vertInfo = vertexBuffer_.info();
		auto indsInfo = indexBuffer_.info();

		vk::cmdBindVertexBuffers(cb, 0,
			{{vertInfo.buffer}}, {{vertInfo.offset}});
		vk::cmdBindIndexBuffer(cb, indsInfo.buffer,
			indsInfo.offset, vk::IndexType::uint32);
	}

	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		context().pipeLayout(), 0, {{ds_}}, {});

	const auto cmdSize = sizeof(vk::DrawIndexedIndirectCommand);
	const auto& indirectCmdBuf = pool_->indirectCmdBuf.buffer();
	const auto pcrStages = vk::ShaderStageBits::vertex;
	auto offset = indirectCmdBuf.offset() + indirectBufID_ * cmdSize;

	if(context().multidrawIndirect()) {
		const auto pcrValue = u32(bindingsBufID_);
		vk::cmdPushConstants(cb, context().pipeLayout(), pcrStages, 8u, 4u, &pcrValue);
		vk::cmdDrawIndexedIndirect(cb, indirectCmdBuf.buffer(), offset, size_, cmdSize);
	} else {
		for(auto i = 0u; i < size_; ++i) {
			const auto pcrValue = u32(bindingsBufID_ + i);
			vk::cmdPushConstants(cb, context().pipeLayout(), pcrStages, 8u, 4u, &pcrValue);
			vk::cmdDrawIndexedIndirect(cb, indirectCmdBuf.buffer(), offset, 1, cmdSize);
			offset += cmdSize;
		}
	}
}

void DrawCall::record(vk::CommandBuffer cb, const vk::Extent2D& viewportSize,
		bool bindPipe, bool bindVertsInds) const {
	dlg_assert(pool_);
	vk::cmdPushConstants(cb, context().pipeLayout(),
		vk::ShaderStageBits::vertex, 0, 8u, &viewportSize);
	record(cb, bindPipe, bindVertsInds);
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
	size_ += draw.size();
	write(id, draw);

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
		indirect[i].indexCount = draw[i].type == DrawType::disabled ? 0 : draw[i].indexCount;
		indirect[i].vertexOffset = draw[i].vertexOffset;
		indirect[i].firstInstance = 0u;
		indirect[i].instanceCount = 1u;

		bindings[i].clipCount = draw[i].clipCount;
		bindings[i].clipStart = draw[i].clipStart;
		bindings[i].transform = draw[i].transform;
		bindings[i].paint = draw[i].paint;
		bindings[i].type = u32(draw[i].type);
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
	draw.type = DrawType(bindings.type);
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

void DrawCall::descriptor(vk::DescriptorSet ds) {
	rerecord_ |= (ds_ != ds);
	ds_ = ds;
}

void DrawCall::vertexBuffer(BufferRef v) {
	rerecord_ |= !(vertexBuffer_ == v);
	vertexBuffer_ = v;
}

void DrawCall::indexBuffer(BufferRef i) {
	rerecord_ |= !(indexBuffer_ == i);
	indexBuffer_ = i;
}

// DrawRecorder
DrawRecorder::DrawRecorder(DrawPool& drawPool, std::vector<DrawCall>& drawCalls,
		std::vector<DrawDescriptor>& descriptors) :
			drawPool_(&drawPool), calls_(&drawCalls), descriptors_(&descriptors) {
	auto& ctx = drawPool.bindingsCmdBuf.context();

	// dummy image as font atlas
	pending_.fontAtlas = &ctx.dummyImageView();

	// no clip by default
	pending_.clipBuffer = ctx.defaultTransform(); // content irrelevant
	clipStart_ = 0u;
	clipCount_ = 0u;

	// identity transform
	pending_.transformBuffer = ctx.defaultTransform();
	transform_ = 0u;

	// dummy paint
	pending_.paintBuffer = ctx.defaultPaint();
	paint_ = 0u;

	current_ = pending_;
}

DrawRecorder::~DrawRecorder() {
	if(!calls_) {
		return;
	}

	flush(); // remaining
	calls_->erase(calls_->begin() + numDraws_, calls_->end());

	// We don't really need this. Might be an optimization in some cases
	// to keep temporarily unused descriptors around.
	if(descriptors_) {
		descriptors_->erase(descriptors_->begin() + numDescriptors_, descriptors_->end());
	}
}

void DrawRecorder::bindTransformBuffer(BufferRef pool) {
	dlg_assert(pool.valid());
	pending_.transformBuffer = pool;
}

void DrawRecorder::bindClipBuffer(BufferRef pool) {
	dlg_assert(pool.valid());
	pending_.clipBuffer = pool;
}

void DrawRecorder::bindPaintBuffer(BufferRef pool) {
	dlg_assert(pool.valid());
	pending_.paintBuffer = pool;
}

void DrawRecorder::bindVertexBuffer(BufferRef pool) {
	dlg_assert(pool.valid());
	pending_.vertexBuffer = pool;
}

void DrawRecorder::bindIndexBuffer(BufferRef pool) {
	dlg_assert(pool.valid());
	pending_.indexBuffer = pool;
}

void DrawRecorder::bindTextures(std::vector<vk::ImageView> textures) {
	pending_.textures = std::move(textures);
}

void DrawRecorder::bindFontAtlas(const vk::ImageView& atlas) {
	pending_.fontAtlas = &atlas;
}

void DrawRecorder::bindTransform(u32 id) {
	transform_ = id;
}
void DrawRecorder::bindPaint(u32 id) {
	paint_ = id;
}
void DrawRecorder::bindClips(u32 start, u32 count) {
	clipStart_ = start;
	clipCount_ = count;
}

void DrawRecorder::flush() {
	if(draws_.empty()) {
		return;
	}

	current_.drawBuffer = drawPool_->bindingsCmdBuf.buffer();

	// find or create descriptor
	vk::DescriptorSet ds {};
	for(auto i = 0u; i < numDescriptors_; ++i) {
		if((*descriptors_)[i].state() == current_) {
			ds = (*descriptors_)[i].ds();
			break;
		}
	}

	if(!ds) {
		if(descriptors_->size() < ++numDescriptors_) {
			descriptors_->emplace_back(drawPool_->bindingsCmdBuf.updateContext());
		}

		auto& nds = (*descriptors_)[numDescriptors_ - 1];
		nds.state(current_);
		ds = nds.ds();
	}

	// add draw
	if(calls_->size() < ++numDraws_) {
		calls_->emplace_back(*drawPool_);
	}
	auto& call = (*calls_)[numDraws_ - 1];
	call.descriptor(ds);
	call.indexBuffer(current_.indexBuffer);
	call.vertexBuffer(current_.vertexBuffer);

	// TODO: kinda hacky. The current DrawCall api is terrible.
	if(call.size() != draws_.size()) {
		call.clear();
		call.add(draws_);
	} else {
		call.write(0, draws_);
	}

	draws_.clear();
}

DrawInstance DrawRecorder::draw(u32 indexStart, u32 indexCount, u32 firstVertex,
		DrawType type, float uvFadeWidth) {
	dlg_assert(indexCount > 0);

	// check if flush is needed
	if(!draws_.empty() && current_ != pending_) {
		flush();
	}

	current_ = pending_;

	// validate state (what is possible at least)
	/*
	dlg_check({
		auto fitsBuffer = [&](auto& buf, auto id, auto count, auto size) {
			if(!count) {
				return true;
			}

			return !count || (
				id != invalid &&
				buf.valid() &&
				(id + count) < buf.allocation->size / size);
		};

		dlg_assert(fitsBuffer(current_.clipBuffer, clipStart_, clipCount_, sizeof(ClipPool::Plane)));
		dlg_assert(fitsBuffer(current_.transformBuffer, transform_, 1, sizeof(TransformPool::Matrix)));
		dlg_assert(fitsBuffer(current_.vertexBuffer, firstVertex, 1, sizeof(Vertex)));
		dlg_assert(fitsBuffer(current_.indexBuffer, indexStart, indexCount, sizeof(Index)));
	});
	*/

	auto& draw = draws_.emplace_back();
	draw.clipStart = clipStart_;
	draw.clipCount = clipCount_;
	draw.transform = transform_;
	draw.paint = paint_;
	draw.uvFadeWidth = uvFadeWidth;
	draw.indexCount = indexCount;
	draw.indexStart = indexStart;
	draw.vertexOffset = firstVertex;
	draw.type = type;

	auto drawID = u32(draws_.size() - 1);
	return {numDraws_, drawID};
}

// DrawDescriptor
DrawDescriptor::DrawDescriptor(UpdateContext& ctx) : DeviceObject(ctx) {
	ds_ = {ctx.dsAllocator(), context().dsLayout()};
}

UpdateFlags DrawDescriptor::updateDevice() {
	vpp::DescriptorSetUpdate dsu(ds_);

	auto validate = [](const auto& info) {
		dlg_assert(info.buffer && info.range);
		return info;
	};

	// TODO: bind dummy buffer as fallback?
	// or warn about them here
	dsu.storage({validate(state_.clipBuffer.info())});
	dsu.storage({validate(state_.transformBuffer.info())});
	dsu.storage({validate(state_.paintBuffer.info())});

	auto& texs = state_.textures;
	auto binding = dsu.currentBinding();
	dlg_assert(texs.size() <= context().numBindableTextures());
	auto sampler = context().sampler();
	auto dummyImage = context().dummyImageView();
	for(auto i = 0u; i < context().numBindableTextures(); ++i) {
		auto tex = dummyImage;
		if(i < texs.size() && texs[i]) {
			tex = texs[i];
		}

		dsu.imageSampler(tex, sampler, binding, i);
	}

	dsu.storage({validate(state_.drawBuffer.info())});
	dlg_assert(state_.fontAtlas && *state_.fontAtlas);
	dsu.imageSampler(*state_.fontAtlas);

	// when descriptor indexing is active, no rerecord is needed
	if(!context().descriptorIndexingFeatures()) {
		return UpdateFlags::rerec;
	}

	// TODO: track what was actually updated. If device e.g. supports
	// texture updates but not storage buffer updates and we only update
	// the texture we don't have to rerecord.
	auto& indexingFeatures = *context().descriptorIndexingFeatures();
	bool rec = indexingFeatures.descriptorBindingSampledImageUpdateAfterBind &&
		indexingFeatures.descriptorBindingStorageBufferUpdateAfterBind;

	return rec ? UpdateFlags::rerec : UpdateFlags::none;
}

// util
void record(vk::CommandBuffer cb, nytl::Span<const DrawCall> draws,
		const vk::Extent2D& extent) {
	auto first = true;
	for(auto& draw : draws) {
		if(first) {
			draw.record(cb, extent);
			first = false;
		} else {
			draw.record(cb, false, false);
		}
	}
}

} // namespace rvg2
