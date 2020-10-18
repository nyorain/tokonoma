#include "buffer.hpp"
#include "context.hpp"
#include <dlg/dlg.hpp>
#include <vpp/vk.hpp>

namespace rvg2 {

u32 resolve(DevMemBits memBits, const vpp::Device& dev) {
	return std::visit(Visitor{
		[](u32 memBits) { return memBits; },
		[&](DevMemType mt) -> u32 {
			switch(mt) {
				case DevMemType::hostVisible: return dev.hostMemoryTypes();
				case DevMemType::deviceLocal: return dev.deviceMemoryTypes();
				default: case DevMemType::all: return 0xFFFFFFFFu;
			}
		}}, memBits);
}

u32 resolve(DevMemBits mb, const vpp::Device& dev, vk::BufferUsageFlags& addFlags) {
	auto ret = resolve(mb, dev);
	// if buffer might be allocated on non-host-visible memory, make sure
	// to include transferDst flag
	// TODO: ignore invalid bits that don't correspond to heap?
	// important e.g. on cards that only have host visible memory
	// when memBits_ is 0xFFFFFFFF
	if((ret & ~dev.hostMemoryTypes()) != 0) {
		addFlags |= vk::BufferUsageBits::transferDst;
	}

	return ret;
}

namespace detail {

void GpuBuffer::init(vk::BufferUsageFlags usage, u32 memBits) {
	usage_ = usage;
	memBits_ = memBits;
}

bool GpuBuffer::updateDevice(UpdateContext& ctx,
		unsigned typeSize, nytl::Span<const std::byte> data) {
	dlg_assertm(memBits_, "GpuBuffer is not valid");

	// check if re-allocation is needed
	auto realloc = false;
	if(buffer_.size() < data.size()) {
		auto size = data.size() * 2; // overallocate
		buffer_ = {ctx.bufferAllocator(), size, usage_, memBits_};
		realloc = true;
	}

	using nytl::write;

	// update
	if(realloc || !updates_.empty()) {
		if(buffer_.mappable()) {
			auto map = buffer_.memoryMap();
			auto span = map.span();

			if(realloc) {
				dlg_trace("writing full mappable buffer, size: {}", data.size());
				write(span, data);
			} else {
				for(auto& update : updates_) {
					auto offset = typeSize * update.start;
					auto size = typeSize * update.count;

					dlg_assert(offset + size <= data.size());
					dlg_trace("update mappable buffer: {} {}", offset, size);
					std::memcpy(map.ptr() + offset, data.data() + offset, size);
				}
			}

			map.flush();
		} else {
			auto cb = ctx.recordableUploadCmdBuf();

			// check how large the stage buf must be
			auto stageSize = 0u;
			if(realloc) {
				stageSize = data.size();
			} else {
				for(auto& update : updates_) {
					stageSize += typeSize * update.count;
				}
			}

			auto stage = vpp::SubBuffer(ctx.bufferAllocator(),
				stageSize, vk::BufferUsageBits::transferSrc,
				ctx.context().device().hostMemoryTypes());
			auto map = stage.memoryMap();
			auto span = map.span();
			std::vector<vk::BufferCopy> copies;
			if(realloc) {
				write(span, data);

				auto& copy = copies.emplace_back();
				copy.size = data.size();
				copy.srcOffset = stage.offset();
				copy.dstOffset = buffer_.offset();
				// dlg_trace("copying full buffer, size: {}", data.size());
			} else {
				auto srcSpan = bytes(data);
				copies.reserve(updates_.size());
				for(auto& update : updates_) {
					auto offset = typeSize * update.start;
					auto size = typeSize * update.count;

					dlg_assert(offset + size <= data.size());
					// dlg_trace("copying buffer: {} {}", offset, size);

					auto& copy = copies.emplace_back();
					copy.size = size;
					copy.srcOffset = stage.offset() + (span.data() - map.ptr());
					copy.dstOffset = buffer_.offset() + offset;
					write(span, srcSpan.subspan(offset, size));
				}

			}

			vk::cmdCopyBuffer(cb, stage.buffer(), buffer_.buffer(), copies);
			ctx.keepAlive(std::move(stage));
		}
	}
	updates_.clear();

	return realloc;
}

unsigned GpuBuffer::allocate(unsigned count) {
	for(auto it = free_.begin(); it != free_.end(); ++it) {
		auto& f = *it;
		auto id = f.start;

		if(f.count == count) {
			free_.erase(it);
			return id;
		} else if(f.count > count) {
			f.start += count;
			f.count -= count;
			return id;
		}
	}

	return 0xFFFFFFFFu;
}

void GpuBuffer::free(unsigned start, unsigned count) {
	// add free blocks
	auto it = std::lower_bound(free_.begin(), free_.end(), start,
		[](auto span, auto start) { return span.start < start; });
	if(it > free_.begin()) {
		// check if we can append to previous blocks
		auto prev = it - 1;
		if(prev->start + prev->count == start) {
			prev->count += count;

			// merge free blocks if possible
			if(!free_.empty() && it < (free_.end() - 1)) {
				auto next = it + 1;
				if(prev->start + prev->count == next->start) {
					prev->count += next->count;
					free_.erase(next);
				}
			}

			return;
		}
	}

	if(!free_.empty() && it < (free_.end() - 1)) {
		// check if we can preprend to previous block
		auto next = it + 1;
		if(start + count == next->start) {
			next->start = start;
			next->count += count;

			// no check for block merging needed.
			// we already know that there is a gap between the previous
			// free block and the currently freed span

			return;
		}
	}

	// new free block needed
	// NOTE: we could not insert it if it's the end of the buffer anyways.
	// Would need that information from the outside though.
	free_.insert(it, {start, count});
}

} // namespace detail
} // namespace rvg
