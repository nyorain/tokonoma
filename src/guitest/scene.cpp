#include "scene.hpp"
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <nytl/bytes.hpp>
#include <dlg/dlg.hpp>

namespace rvg {

// util
namespace {

template<typename T>
nytl::Span<T> asSpan(nytl::Span<std::byte> bytes) {
	dlg_assert(bytes.size() % sizeof(T) == 0);
	auto size = bytes.size() / sizeof(T);
	auto ptr = reinterpret_cast<T*>(bytes.data());
	return nytl::Span<T>(ptr, size);
}

template<typename T>
nytl::Span<const T> asSpan(nytl::Span<const std::byte> bytes) {
	dlg_assert(bytes.size() % sizeof(T) == 0);
	auto size = bytes.size() / sizeof(T);
	auto ptr = reinterpret_cast<const T*>(bytes.data());
	return nytl::Span<const T>(ptr, size);
}

template<typename T>
unsigned bufferOffsetToID(unsigned offset) {
	dlg_assert(offset % sizeof(T) == 0u);
	return offset / sizeof(T);
}

template<typename T>
unsigned bufferIDToOffset(unsigned id) {
	return id * sizeof(T);
}

} // anon namespace

// Buffer
Buffer::Buffer(rvg::Context& ctx, vk::BufferUsageFlags usage, u32 memBits) :
		context_(&ctx), usage_(usage), memBits_(memBits) {
	// if buffer might be allocated non non-host-visible memory, we need
	// to add the transferDst usage flag
	if((memBits_ & (~ctx.device().hostMemoryTypes())) != 0) {
		usage_ |= vk::BufferUsageBits::transferDst;
	}
}

unsigned Buffer::allocate(unsigned size) {
	for(auto it = free_.begin(); it != free_.end(); ++it) {
		auto& f = *it;
		auto off = f.offset;

		if(f.size == size) {
			free_.erase(it);
		} else {
			f.offset += size;
			f.size -= size;
		}

		return off;
	}

	// append
	auto off = data_.size();
	data_.resize(data_.size() + size);
	return off;
}

void Buffer::free(unsigned offset, unsigned size) {
	// check if data size can be reduced since this was the last block
	if(offset + size == data_.size()) {
		data_.resize(offset);
	}

	// add free blocks
	auto it = std::lower_bound(free_.begin(), free_.end(), offset,
		[](auto span, auto offset) { return span.offset < offset; });
	if(it > free_.begin()) {
		// check if we can append to previous blocks
		auto prev = it - 1;
		if(prev->offset + prev->size == offset) {
			prev->size += size;

			// merge free blocks if possible
			if(!free_.empty() && it < (free_.end() - 1)) {
				auto next = it + 1;
				if(prev->offset + prev->size == next->offset) {
					prev->size += next->size;
					free_.erase(next);
				}
			}

			return;
		}
	}

	if(!free_.empty() && it < (free_.end() - 1)) {
		// check if we can preprend to previous block
		auto next = it + 1;
		if(offset + size == next->offset) {
			next->offset = offset;
			next->size += size;

			// no check for block merging needed.
			// we already know that there is a gap between the previous
			// free block and the currently freed span

			return;
		}
	}

	// new free block needed
	free_.insert(it, {offset, size});
}

void Buffer::write(unsigned offset, nytl::Span<const std::byte> src) {
	dlg_assert(offset + src.size() <= data_.size());
	std::memcpy(data_.data() + offset, src.data(), src.size());

	// we don't have to check for already existing updates for this area.
	// in that case the copying will simply be done twice. That case should
	// happen very rarely, does not justify the overhead and additional code to
	// optimize for it.
	updates_.push_back({offset, src.size()});
}

unsigned Buffer::allocate(nytl::Span<const std::byte> data) {
	auto off = allocate(data.size());
	write(off, data);
	return off;
}

nytl::Span<std::byte> Buffer::writable(unsigned offset, unsigned size) {
	dlg_assert(offset + size <= data_.size());
	updates_.push_back({offset, size});
	return nytl::bytes(data_).subspan(offset, size);
}

bool Buffer::updateDevice() {
	// check if re-allocation is needed
	auto realloc = false;
	if(buffer_.size() < data_.size()) {
		auto size = data_.size() * 2; // overallocate
		buffer_ = {context().bufferAllocator(), size, usage_, memBits_};
		realloc = true;
	}

	using nytl::write;

	// update
	if(realloc || !updates_.empty()) {
		if(buffer_.mappable()) {
			auto map = buffer_.memoryMap();
			auto span = map.span();

			if(realloc) {
				dlg_trace("writing full mappable buffer, size: {}", data_.size());
				write(span, data_);
			} else {
				for(auto& update : updates_) {
					dlg_assert(update.offset + update.size <= data_.size());
					dlg_trace("update mappable buffer: {} {}", update.offset, update.size);
					std::memcpy(map.ptr() + update.offset,
						data_.data() + update.offset,
						update.size);
				}
			}

			map.flush();
		} else {
			auto& ctx = context();
			auto cb = ctx.uploadCmdBuf();

			// check how large the stage buf must be
			auto stageSize = 0u;
			if(realloc) {
				stageSize = data_.size();
			} else {
				for(auto& update : updates_) {
					stageSize += update.size;
				}
			}

			auto stage = vpp::SubBuffer(ctx.bufferAllocator(),
				stageSize, vk::BufferUsageBits::transferSrc,
				ctx.device().hostMemoryTypes());
			auto map = stage.memoryMap();
			auto span = map.span();
			std::vector<vk::BufferCopy> copies;
			if(realloc) {
				write(span, data_);

				auto& copy = copies.emplace_back();
				copy.size = data_.size();
				copy.srcOffset = stage.offset();
				copy.dstOffset = buffer_.offset();
				dlg_trace("copying full buffer, size: {}", data_.size());
			} else {
				auto srcSpan = bytes(data_);
				copies.reserve(updates_.size());
				for(auto& update : updates_) {
					dlg_trace("copying buffer: {} {}", update.offset, update.size);
					write(span, srcSpan.subspan(update.offset, update.size));
					auto& copy = copies.emplace_back();
					copy.size = update.size;
					copy.srcOffset = stage.offset() + span.data() - map.ptr();
					copy.dstOffset = buffer_.offset() + update.offset;
				}

			}

			vk::cmdCopyBuffer(cb, stage.buffer(), buffer_.buffer(), copies);
			ctx.addStage(std::move(stage));
			ctx.addCommandBuffer({}, std::move(cb)); // TODO
		}
	}
	updates_.clear();

	return realloc;
}

// SizeTrackedBuffer
unsigned AllocTrackedBuffer::allocate(unsigned size) {
	auto off = Buffer::allocate(size);
	auto it = std::lower_bound(allocations_.begin(), allocations_.end(), off,
		[](auto span, auto offset) { return span.offset < offset; });
	allocations_.insert(it, {off, size});
	return off;
}

void AllocTrackedBuffer::free(unsigned offset) {
	auto it = std::lower_bound(allocations_.begin(), allocations_.end(), offset,
		[](auto span, auto offset) { return span.offset < offset; });
	dlg_assertm(it != allocations_.end() && it->offset == offset,
		"No allocation for given offset");
	Buffer::free(it->offset, it->size);
	allocations_.erase(it);
}

void AllocTrackedBuffer::free(unsigned offset, unsigned size) {
	// debug check that allocation exists
	auto it = std::lower_bound(allocations_.begin(), allocations_.end(), offset,
		[](auto span, auto offset) { return span.offset < offset; });
	dlg_assertm(it != allocations_.end() &&
		it->offset == offset &&
		it->size == size,
		"No allocation for given offset and size");
	Buffer::free(offset, size);
	allocations_.erase(it);
}

// VertexPool
VertexPool::VertexPool(rvg::Context& ctx) :
	vertices_(ctx, vk::BufferUsageBits::vertexBuffer, ctx.device().deviceMemoryTypes()),
	indices_(ctx, vk::BufferUsageBits::indexBuffer, ctx.device().deviceMemoryTypes()) {
}

void VertexPool::bind(vk::CommandBuffer cb) {
	auto& vb = vertices_.buffer();
	auto& ib = indices_.buffer();
	vk::cmdBindVertexBuffers(cb, 0, {{vb.buffer()}}, {{vb.offset()}});
	vk::cmdBindIndexBuffer(cb, ib.buffer(), ib.offset(), vk::IndexType::uint32);
}

bool VertexPool::updateDevice() {
	auto ret = false;
	ret |= vertices_.updateDevice();
	ret |= indices_.updateDevice();
	return ret;
}

unsigned VertexPool::allocateVertices(unsigned count) {
	return bufferOffsetToID<Vertex>(vertices_.allocate(count * sizeof(Vertex)));
}
unsigned VertexPool::allocateIndices(unsigned count) {
	return bufferOffsetToID<Index>(indices_.allocate(count * sizeof(Index)));
}
void VertexPool::writeVertices(unsigned id, Span<const Vertex> vertices) {
	auto offset = bufferIDToOffset<Vertex>(id);
	vertices_.write(offset, nytl::bytes(vertices));
}
void VertexPool::writeIndices(unsigned id, Span<const Index> indices) {
	auto offset = bufferIDToOffset<Index>(id);
	indices_.write(offset, nytl::bytes(indices));
}
void VertexPool::freeVertices(unsigned id) {
	auto offset = bufferIDToOffset<Vertex>(id);
	vertices_.free(offset);
}
void VertexPool::freeIndices(unsigned id) {
	auto offset = bufferIDToOffset<Index>(id);
	indices_.free(offset);
}

// TransformPool
TransformPool::TransformPool(Context& ctx) :
	buffer_(ctx, vk::BufferUsageBits::storageBuffer, ctx.device().deviceMemoryTypes()) {
}

unsigned TransformPool::allocate() {
	auto offset = buffer_.allocate(sizeof(Matrix));
	auto id = bufferOffsetToID<Matrix>(offset);
	new(buffer_.writable(offset, sizeof(Matrix)).data()) Matrix();
	return id;
}

void TransformPool::free(unsigned id) {
	auto offset = bufferIDToOffset<Matrix>(id);
	buffer_.free(offset, sizeof(Matrix));
}

void TransformPool::write(unsigned id, const Matrix& transform) {
	auto offset = bufferIDToOffset<Matrix>(id);
	buffer_.write(offset, nytl::bytes(transform));
}

TransformPool::Matrix& TransformPool::writable(unsigned id) {
	auto offset = bufferIDToOffset<Matrix>(id);
	return nytl::ref<Matrix>(buffer_.writable(offset, sizeof(Matrix)));
}

// ClipPool
ClipPool::ClipPool(Context& ctx) :
	buffer_(ctx, vk::BufferUsageBits::storageBuffer, ctx.device().deviceMemoryTypes()) {
}

unsigned ClipPool::allocate(unsigned count) {
	return bufferOffsetToID<Plane>(buffer_.allocate(sizeof(Plane) * count));
}

void ClipPool::free(unsigned id, unsigned count) {
	auto offset = bufferIDToOffset<Plane>(id);
	buffer_.free(offset, sizeof(Plane) * count);
}

void ClipPool::write(unsigned id, nytl::Span<const Plane> planes) {
	auto offset = bufferIDToOffset<Plane>(id);
	buffer_.write(offset, nytl::bytes(planes));
}

// PaintPool
PaintPool::PaintPool(Context& ctx) :
	buffer_(ctx, vk::BufferUsageBits::storageBuffer, ctx.device().deviceMemoryTypes()) {
}

unsigned PaintPool::allocate() {
	return bufferOffsetToID<PaintData>(buffer_.allocate(sizeof(PaintData)));
}
void PaintPool::free(unsigned id) {
	auto offset = bufferIDToOffset<PaintData>(id);
	buffer_.free(offset, sizeof(PaintData));
}

void PaintPool::write(unsigned id, const PaintData& data) {
	auto offset = bufferIDToOffset<PaintData>(id);
	buffer_.write(offset, nytl::bytes(data));
}
void PaintPool::setTexture(unsigned i, vk::ImageView texture) {
	dlg_assert(i < numTextures);
	textures_.resize(std::max<std::size_t>(i, textures_.size()));
	textures_[i] = texture;
}

// Scene
Scene::Scene(Context& ctx) : context_(&ctx) {
	const auto stages = vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment;
	std::array<vk::Sampler, numTextures> samplers;
	for(auto& s : samplers) {
		s = context().textureSampler();
	}

	auto& dev = context().device();
	auto bindings = std::array{
		// clip
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// transform
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// paint, buffer + textures
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler, stages,
			samplers.data(), numTextures),
		// draw commands
		vpp::descriptorBinding(vk::DescriptorType::storageBuffer, stages),
		// font atlas
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler, stages,
			&context().textureSampler().vkHandle()),
	};
	dsl_.init(dev, bindings);

	if(context().multidrawIndirect()) {
		pipeLayout_ = {dev, {{dsl_.vkHandle()}}, {}};
	} else {
		vk::PushConstantRange pcr;
		pcr.offset = 0u;
		pcr.size = 4u;
		pcr.stageFlags = vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment;
		pipeLayout_ = {dev, {{dsl_.vkHandle()}}, {{pcr}}};
	}
}

bool Scene::updateDevice() {
	auto rerec = false;
	auto ownCmdBufChanged = false;

	// update commands
	if(!cmdUpdates_.empty()) {
		// ensure size
		// indirectCmdBuffer
		auto needed = sizeof(vk::DrawIndexedIndirectCommand) * numDrawCalls_;
		if(indirectCmdBuffer_.size() < needed) {
			needed = std::max<vk::DeviceSize>(needed * 2, 16 * sizeof(vk::DrawIndexedIndirectCommand));
			auto usage = vk::BufferUsageBits::indirectBuffer;
			indirectCmdBuffer_ = {context().bufferAllocator(),
				needed, usage, context().device().hostMemoryTypes()};
			rerec = true;
		}

		// ownCmdBuffer
		needed = sizeof(DrawCommandData) * numDrawCalls_;
		if(ownCmdBuffer_.size() < needed) {
			needed = std::max<vk::DeviceSize>(needed * 2, 16 * sizeof(DrawCommandData));
			auto usage = vk::BufferUsageBits::storageBuffer;
			ownCmdBuffer_ = {context().bufferAllocator(),
				needed, usage, context().device().hostMemoryTypes()};
			ownCmdBufChanged = true;
			rerec = true;
		}

		// maps
		// TODO(perf): make maps persistent?
		auto indirectCmdMap = indirectCmdBuffer_.memoryMap();
		auto indirectCmds = asSpan<vk::DrawIndexedIndirectCommand>(indirectCmdMap.span());

		auto ownCmdMap = ownCmdBuffer_.memoryMap();
		auto ownCmds = asSpan<DrawCommandData>(ownCmdMap.span());

		// write updates
		for(auto& update : cmdUpdates_) {
			auto& srcCmd = draws_[update.draw].commands[update.command];
			indirectCmds[update.offset].firstInstance = 0u;
			indirectCmds[update.offset].instanceCount = 1u;
			indirectCmds[update.offset].firstIndex = srcCmd.indexStart;
			indirectCmds[update.offset].indexCount = srcCmd.indexCount;
			indirectCmds[update.offset].vertexOffset = 0u;

			ownCmds[update.offset].transform = srcCmd.transform;
			ownCmds[update.offset].paint = srcCmd.paint;
			ownCmds[update.offset].clipStart = srcCmd.clipStart;
			ownCmds[update.offset].clipCount = srcCmd.clipCount;
			ownCmds[update.offset].type = srcCmd.type;
			ownCmds[update.offset].uvFadeWidth = srcCmd.uvFadeWidth;
		}

		indirectCmdMap.flush();
		ownCmdMap.flush();
		cmdUpdates_.clear();
	}

	// update descriptors
	std::vector<vpp::DescriptorSetUpdate> dsus;
	auto updateDs = [&](auto& draw) {
		auto& dsu = dsus.emplace_back(draw.ds);
		dsu.storage(draw.clipPool->buffer());
		dsu.storage(draw.transformPool->buffer());
		dsu.storage(draw.paintPool->buffer());

		auto& texs = draw.paintPool->textures();
		auto binding = dsu.currentBinding();
		dlg_assert(texs.size() <= numTextures);
		for(auto i = 0u; i < texs.size(); ++i) {
			dsu.imageSampler(texs[i], {}, binding, i);
		}

		auto& emptyImage = context().emptyImage();
		for(auto i = texs.size(); i < numTextures; ++i) {
			dsu.imageSampler(emptyImage.vkImageView(), {}, binding, i);
		}

		dsu.storage(ownCmdBuffer_);
		dsu.imageSampler(draw.fontAtlas);
	};

	if(ownCmdBufChanged) {
		for(auto& draw : draws_) {
			updateDs(draw);
		}

		dsUpdates_.clear(); // make sure they are not updated again
	}

	if(!dsUpdates_.empty()) {
		for(auto& dsUpdate : dsUpdates_) {
			updateDs(draws_[dsUpdate]);
		}

		dsUpdates_.clear();
		if(!dynamicDescriptors()) {
			rerec = true;
		}
	}

	vpp::apply(dsus);
	return rerec;
}

void Scene::recordDraw(vk::CommandBuffer cb) {
	const auto multidrawIndirect = context().multidrawIndirect();

	const auto cmdSize = sizeof(vk::DrawIndexedIndirectCommand);
	auto offset = indirectCmdBuffer_.offset();
	for(auto& draw : draws_) {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {{draw.ds.vkHandle()}}, {});
		draw.vertexPool->bind(cb);

		if(multidrawIndirect) {
			vk::cmdDrawIndexedIndirect(cb, indirectCmdBuffer_.buffer(), offset,
				draw.commands.size(), cmdSize);
			offset += cmdSize * draw.commands.size();
		} else {
			for(auto i = 0u; i < draw.commands.size(); ++i) {
				const auto pcrValue = u32(i);
				const auto pcrStages =
					vk::ShaderStageBits::fragment |
					vk::ShaderStageBits::vertex;

				vk::cmdPushConstants(cb, pipeLayout_, pcrStages, 0, 4u, &pcrValue);
				vk::cmdDrawIndexedIndirect(cb, indirectCmdBuffer_.buffer(), offset,
					1, cmdSize);
				offset += cmdSize;
			}
		}
	}
}

void Scene::add(DrawSet set) {
	auto id = numDrawCalls_;
	if(draws_.size() <= id) {
		dlg_assert(draws_.size() == id);
		auto& draw = draws_.emplace_back(std::move(set));
		// TODO: shouldn't reallocate every time the number of draws
		// is resized, e.g. think of a case where something is hidden
		// and shown again over and over (meaning in one frame a draw call is
		// added, then not), we allocate a new ds every time.
		draw.ds = {context().dsAllocator(), dsl_};
		dsUpdates_.push_back(id);

		for(auto c = 0u; c < draw.commands.size(); ++c) {
			auto& update = cmdUpdates_.emplace_back();
			update.draw = id;
			update.command = c;
			update.offset = numDrawCalls_;
			draw.commands[c].offset = numDrawCalls_;

			++numDrawCalls_;
		}
	} else {
		// check if descriptors are the same
		auto& oldDraw = draws_[id];
		bool same =
			oldDraw.transformPool == set.transformPool &&
			oldDraw.clipPool == set.clipPool &&
			oldDraw.paintPool == set.paintPool &&
			oldDraw.vertexPool == set.vertexPool &&
			oldDraw.fontAtlas == set.fontAtlas;
		if(!same) {
			dsUpdates_.push_back(id);
		}

		// compare each draw command
		oldDraw.commands.resize(set.commands.size());
		for(auto c = 0u; c < set.commands.size(); ++c) {
			auto& oldc = oldDraw.commands[c];
			auto& newc = set.commands[c];
			newc.offset = numDrawCalls_;
			if(std::memcmp(&oldc, &newc, sizeof(oldc))) {
				// the commands aren't the same, need update
				auto& update = cmdUpdates_.emplace_back();
				update.draw = id;
				update.command = c;
				update.offset = numDrawCalls_;
			}

			++numDrawCalls_;
		}
	}

	++numDraws_;
}

// DrawRecorder
DrawRecorder::DrawRecorder(Scene& scene) : scene_(scene) {
}

DrawRecorder::~DrawRecorder() {
	if(!current_.commands.empty()) {
		scene_.add(std::move(current_));
	}
	scene_.finish();
}

void DrawRecorder::bind(TransformPool& pool) {
	pending_.transformPool = &pool;
	if(current_.commands.empty()) {
		current_.transformPool = &pool;
	}
}

void DrawRecorder::bind(ClipPool& pool) {
	pending_.clipPool = &pool;
	if(current_.commands.empty()) {
		current_.clipPool = &pool;
	}
}

void DrawRecorder::bind(PaintPool& pool) {
	pending_.paintPool = &pool;
	if(current_.commands.empty()) {
		current_.paintPool = &pool;
	}
}

void DrawRecorder::bind(VertexPool& pool) {
	pending_.vertexPool = &pool;
	if(current_.commands.empty()) {
		current_.vertexPool = &pool;
	}
}

void DrawRecorder::bindFontAtlas(vk::ImageView atlas) {
	pending_.fontAtlas = atlas;
	if(current_.commands.empty()) {
		current_.fontAtlas = atlas;
	}
}

void DrawRecorder::draw(const DrawCall& call) {
	// check if state is still the same
	dlg_assert(pending_.commands.empty()); // TODO: shouldn't exist in first place
	bool same =
		(current_.clipPool == pending_.clipPool) &&
		(current_.transformPool == pending_.transformPool) &&
		(current_.paintPool == pending_.paintPool) &&
		(current_.vertexPool == pending_.vertexPool) &&
		(current_.fontAtlas == pending_.fontAtlas);
	if(!same) {
		dlg_assert(!current_.commands.empty());
		scene_.add(std::move(current_));
		current_ = std::move(pending_);
	}

	DrawCommand cmd;
	static_cast<DrawCall&>(cmd) = call;
	current_.commands.push_back(cmd);
}

} // namespace rvg

