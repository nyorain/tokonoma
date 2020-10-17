#pragma once

#include <vpp/fwd.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/bytes.hpp>
#include <nytl/span.hpp>
#include <cstdint>
#include <algorithm>

namespace rvg2 {

class Context;
using u32 = std::uint32_t;
static constexpr auto invalid = u32(0xFFFFFFFFu);

enum class DevMemType {
	all,
	hostVisible,
	deviceLocal,
};

using DevMemBits = std::variant<u32, DevMemType>;

namespace detail {

struct BufferSpan {
	vk::DeviceSize start;
	vk::DeviceSize count;
};

// TODO: add reserve, shrink mechanisms
// TODO: add more intelligent realloc here
// TODO: more intelligent over-allocation, also make sure we never
//   allocate buffer sizes below a certain threshold
// TODO: support deferred initialization
// TODO: don't rely on typeSize? make this a pure byte buffer?
struct GpuBuffer {
	// the associated context, needed e.g. for allocators
	Context* context_;
	// usage and memBits are stored for when the buffer needs to be recreated
	vk::BufferUsageFlags usage_;
	u32 memBits_;
	// ranges that have to be updated
	std::vector<BufferSpan> updates_;
	// ranges that are still free
	std::vector<BufferSpan> free_;
	vpp::SubBuffer buffer_;


	GpuBuffer(Context& ctx, vk::BufferUsageFlags usage, DevMemBits memBits);
	u32 allocate(u32 count);
	void free(u32 id, u32 count);
	bool updateDevice(u32 typeSize, nytl::Span<const std::byte> data);
};

} // namespace detail

template<typename T>
class Buffer {
public:
	using Type = T;

public:
	Buffer(Context& ctx, vk::BufferUsageFlags usage, DevMemBits memBits) :
		buffer_(ctx, usage, memBits) {
	}

	// TODO: revisit/fix
	~Buffer() = default;

	Buffer(Buffer&&) = delete;
	Buffer& operator=(Buffer&&) = delete;

	unsigned allocate(unsigned count = 1) {
		auto id = buffer_.allocate(count);
		if(id == invalid) {
			id = data_.size();
		}
		data_.resize(std::max<unsigned>(data_.size(), id + count));
		return id;
	}

	unsigned realloc(unsigned startID, unsigned count, unsigned newCount) {
		// TODO: more intelligent algorithm that first checks whether
		// it can be extended.
		if(!count) {
			return allocate(newCount);
		}

		// First, get a new logcial allocation on the buffer.
		// Important to free first, so that the allocation will just
		// be extended in-place if possible.
		buffer_.free(startID, count);
		auto id = buffer_.allocate(newCount);
		if(id == invalid) {
			id = data_.size();
		}

		// Check whether this allocation was the last in our logical state.
		if(startID + count == data_.size()) {
			// If so, we resize our logical state to tightly fit the new
			// allocation.
			auto newEnd = (id == data_.size()) ? (startID + newCount) : startID;
			data_.resize(newEnd);
		} else {
			// Otherwise, just make sure our logical state is large enough.
			data_.resize(std::max<unsigned>(data_.size(), id + newCount));
		}

		// Now copy from the old allocation in our logical state to the
		// new allocation.
		auto mcount = std::min(count, newCount);
		auto b = data_.data();
		// We don't use std::copy since our ranges may overlap in any way.
		// std::memmove guarantees out it works, and Buffer is designed
		// for trivial types anyways.
		// std::copy(b + id, b + id + mcount, b + startID);
		std::memmove(b + id, b + startID, mcount);
		return id;
	}

	void reallocRef(unsigned& startID, unsigned& count, unsigned newCount) {
		unsigned i = realloc(startID, count, newCount);
		startID = i;
		count = newCount;
	}

	void write(unsigned startID, nytl::Span<const T> data) {
		buffer_.updates_.push_back({startID, data.size()});
		NYTL_BYTES_ASSERT(data_.size() >= startID + data.size());
		std::copy(data.begin(), data.end(), data_.begin() + startID);
	}

	void write(unsigned startID, const T& data) {
		write(startID, nytl::Span<const T>(&data, 1));
	}

	unsigned create(const T& data) {
		auto id = allocate(1);
		write(id, data);
		return id;
	}

	unsigned create(nytl::Span<const T> data) {
		auto id = allocate(data.size());
		write(id, data);
		return id;
	}

	void free(unsigned startID, unsigned count) {
		// check if data size can be reduced since this was the last block
		buffer_.free(startID, count);
		if(startID + count == data_.size()) {
			data_.resize(startID);
		}
	}

	// Returns the given span as writable buffer.
	// The buffer is only guaranteed to be valid until the allocate or free
	// call. Will automatically mark the returned range for update.
	nytl::Span<T> writable(unsigned startID, unsigned count) {
		NYTL_BYTES_ASSERT(data_.size() >= startID + count);
		buffer_.updates_.push_back({startID, count});
		auto full = nytl::span(data_);
		return full.subspan(startID, count);
	}

	T& writable(unsigned id) {
		NYTL_BYTES_ASSERT(data_.size() > id);
		buffer_.updates_.push_back({id, 1u});
		return data_[id];
	}

	const T& read(unsigned id) const {
		NYTL_BYTES_ASSERT(data_.size() > id);
		return data_[id];
	}

	nytl::Span<const T> read(unsigned startID, unsigned count) const {
		NYTL_BYTES_ASSERT(data_.size() >= startID + count);
		auto full = nytl::span(data_);
		return full.subspan(startID, count);
	}

	bool updateDevice() {
		return buffer_.updateDevice(sizeof(T), nytl::bytes(data_));
	}

	Context& context() const { return *buffer_.context_; }
	const vpp::SubBuffer& buffer() const { return buffer_.buffer_; }
	const std::vector<T>& data() const { return data_; }
	std::size_t logicalSize() const { return data_.size(); }

protected:
	using Span = detail::BufferSpan;
	detail::GpuBuffer buffer_;
	std::vector<T> data_;
};


/*
// Buffer that tracks allocation and therefore does not require the size
// of the allocation when freeing.
template<typename T>
class AllocTrackedBuffer : public Buffer<T> {
public:
	using Buffer<T>::Buffer;
	unsigned allocate(unsigned count) {
		auto off = Buffer<T>::allocate(count);
		auto it = std::lower_bound(allocations_.begin(), allocations_.end(), off,
			[](auto span, auto offset) { return span.offset < offset; });
		allocations_.insert(it, {off, count});
		return off;
	}

	void free(unsigned start) {
		auto it = std::lower_bound(allocations_.begin(), allocations_.end(), start,
			[](auto span, auto start) { return span.start < start; });
		assert(it != allocations_.end() && it->start == start &&
			"No allocation for given offset");
		Buffer<T>::free(it->start, it->count);
		allocations_.erase(it);
	}

	void free(unsigned start, unsigned count) {
		// debug check that allocation exists
		auto it = std::lower_bound(allocations_.begin(), allocations_.end(), start,
			[](auto span, auto start) { return span.start < start; });
		assert(it != allocations_.end() &&
			it->start == start &&
			it->count == count &&
			"No allocation for given offset and size");
		Buffer<T>::free(start, count);
		allocations_.erase(it);
	}

protected:
	std::vector<detail::BufferSpan> allocations_;
};
*/

} // namespace rvg
