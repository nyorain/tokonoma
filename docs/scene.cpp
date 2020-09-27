#include "scene.hpp"
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <nytl/bytes.hpp>
#include <dlg/dlg.hpp>

namespace rvg {

// util
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

// Buffer
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

void Buffer::fill(unsigned offset, nytl::Span<const std::byte> src) {
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
	fill(off, data);
	return off;
}

bool Buffer::updateDevice() {
	// check if re-allocation is needed
	auto realloc = false;
	if(buffer_.size() < data_.size()) {
		auto size = data_.size() * 2; // overallocate
		buffer_ = {context().bufferAllocator(), size,
			vk::BufferUsageBits::vertexBuffer,
			context().device().deviceMemoryTypes()};
		realloc = true;
	}

	// update
	if(realloc || !updates_.empty()) {
		if(buffer_.mappable()) {
			auto map = buffer_.memoryMap();
			auto span = map.span();

			if(realloc) {
				write(span, data_);
			} else {
				for(auto& update : updates_) {
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
				stageSize, vk::BufferUsageBits::transferSrc);
			auto map = stage.memoryMap();
			auto span = map.span();
			if(realloc) {
				write(span, data_);
			} else {
				auto srcSpan = bytes(data_);
				std::vector<vk::BufferCopy> copies;
				copies.reserve(updates_.size());
				for(auto& update : updates_) {
					write(span, srcSpan.subspan(update.offset, update.size));
					auto copy = copies.emplace_back();
					copy.size = update.size;
					copy.srcOffset = span.data() - map.ptr();
					copy.dstOffset = buffer_.offset() + update.offset;
				}

				vk::cmdCopyBuffer(cb, stage.buffer(), buffer_.buffer(), copies);

				ctx.addStage(std::move(stage));
				ctx.addCommandBuffer({}, std::move(cb)); // TODO
			}
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
void VertexPool::bind(vk::CommandBuffer cb) {
	vk::cmdBindVertexBuffers(cb, 0,
		{{vertexBuffer_.buffer()}}, {{vertexBuffer_.offset()}});
	vk::cmdBindIndexBuffer(cb, indexBuffer_.buffer(), indexBuffer_.offset(),
		vk::IndexType::uint32);
}

bool VertexPool::updateDevice() {
	auto ret = false;

	// check if re-allocation is needed
	auto writeAllVerts = false;
	if(vertexBuffer_.size() < vertices_.size()) {
		auto size = vertices_.size() * 2;
		vertexBuffer_ = {context().bufferAllocator(), size,
			vk::BufferUsageBits::vertexBuffer,
			context().device().deviceMemoryTypes()};
		writeAllVerts = true;
		ret = true;
	}

	auto writeAllInds = false;
	if(indexBuffer_.size() < indices_.size()) {
		auto size = indices_.size() * 2;
		vertexBuffer_ = {context().bufferAllocator(), size,
			vk::BufferUsageBits::indexBuffer,
			context().device().deviceMemoryTypes()};
		writeAllInds = true;
		ret = true;
	}

	// write updates
	// vertices
	if(!vertexUpdates_.empty() || writeAllVerts) {
		if(vertexBuffer_.mappable()) {
			auto map = vertexBuffer_.memoryMap();
			auto span = map.span();

			if(writeAllVerts) {
				write(span, vertices_);
			} else {
				for(auto& update : vertexUpdates_) {
					std::memcpy(map.ptr() + update.start,
						vertices_.data() + update.start,
						update.bytes);
				}
			}

			map.flush();
		} else {
			auto& ctx = context();
			auto cb = ctx.uploadCmdBuf();

			// check how large the stage buf must be
			auto stageSize = writeAllVerts ? vertices_.size() : 0u;
			if(!writeAllVerts) {
				for(auto& update : vertexUpdates_) {
					stageSize += update.bytes;
				}
			}

			auto stage = vpp::SubBuffer(ctx.bufferAllocator(),
				stageSize, vk::BufferUsageBits::transferSrc);
			auto map = stage.memoryMap();
			auto span = map.span();
			if(writeAllVerts) {
				write(span, vertices_);
			} else {
				auto vertexSpan = bytes(vertices_);
				std::vector<vk::BufferCopy> copies;
				copies.reserve(vertexUpdates_.size());
				for(auto& update : vertexUpdates_) {
					write(span, vertexSpan.subspan(update.start, update.bytes));
					auto copy = copies.emplace_back();
					copy.size = update.bytes;
					copy.srcOffset = span.data() - map.ptr();
					copy.dstOffset = vertexBuffer_.offset() + update.start;
				}

				vk::cmdCopyBuffer(cb, stage.buffer(), vertexBuffer_.buffer(), copies);

				ctx.addStage(std::move(stage));
				ctx.addCommandBuffer({}, std::move(cb)); // TODO
			}
		}
	}
	vertexUpdates_.clear();

	// indices

	return ret;
}

// Scene
bool Scene::updateDevice() {
	auto rerec = false;
	auto ownCmdBufChanged = false;

	// update commands
	if(!cmdUpdates_.empty()) {
		// ensure size
		// indirectCmdBuffer
		auto needed = sizeof(vk::DrawIndirectCommand) * numDrawCalls_;
		if(indirectCmdBuffer_.size() < needed) {
			needed = std::max<vk::DeviceSize>(needed * 2, 1024u);
			auto usage = vk::BufferUsageBits::indirectBuffer;
			indirectCmdBuffer_ = {context().bufferAllocator(),
				needed, usage, context().device().hostMemoryTypes()};
			rerec = true;
		}

		// ownCmdBuffer
		needed = sizeof(DrawCommandData) * numDrawCalls_;
		if(ownCmdBuffer_.size() < needed) {
			needed = std::max<vk::DeviceSize>(needed * 2, 1024u);
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
		for(auto i = 0u; i < texs.size(); ++i) {
			dsu.imageSampler(texs[i], {}, -1, i);
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
	const auto pipeLayout = vk::PipelineLayout(context().pipeLayout());

	const auto cmdSize = sizeof(vk::DrawIndexedIndirectCommand);
	auto offset = indirectCmdBuffer_.offset();
	for(auto& draw : draws_) {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout, 0, {{draw.ds.vkHandle()}}, {});
		draw.vertexPool->bind(cb);

		if(multidrawIndirect) {
			vk::cmdDrawIndexedIndirect(cb, indirectCmdBuffer_.buffer(), offset,
				draw.commands.size(), cmdSize);
			offset += cmdSize * draw.commands.size();
		} else {
			for(auto i = 0u; i < draw.commands.size(); ++i) {
				vk::cmdDrawIndexedIndirect(cb, indirectCmdBuffer_.buffer(), offset,
					1, cmdSize);
				offset += cmdSize;
			}
		}
	}
}

void Scene::set(unsigned i, DrawSet set) {
	if(draws_.size() <= i) {
		dlg_assert(draws_.size() == i);
		auto& draw = draws_.emplace_back(std::move(set));
		dsUpdates_.push_back(i);

		for(auto c = 0u; c < draw.commands.size(); ++c) {
			auto& update = cmdUpdates_.emplace_back();
			update.draw = i;
			update.command = c;
			update.offset = numDrawCalls_;
			draw.commands[c].offset = numDrawCalls_;

			++numDrawCalls_;
		}
	} else {
		// check if descriptors are the same
		auto& oldDraw = draws_[i];
		bool same =
			oldDraw.transformPool == set.transformPool &&
			oldDraw.clipPool == set.clipPool &&
			oldDraw.paintPool == set.paintPool &&
			oldDraw.vertexPool == set.vertexPool &&
			oldDraw.fontAtlas == set.fontAtlas;
		if(!same) {
			dsUpdates_.push_back(i);
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
				update.draw = i;
				update.command = c;
				update.offset = numDrawCalls_;
			}

			++numDrawCalls_;
		}
	}

	++numDraws_;
	dlg_assert(numDraws_ == i);
}

} // namespace rvg
