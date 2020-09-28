#pragma once

#include <rvg/fwd.hpp>
#include <rvg/context.hpp>
#include <vpp/fwd.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/bytes.hpp>
#include <nytl/span.hpp>
#include <cstdint>
#include <algorithm>

namespace rvg {

using u32 = std::uint32_t;

namespace detail {

struct BufferSpan {
	vk::DeviceSize start;
	vk::DeviceSize count;
};

struct GpuBuffer {
	rvg::Context* context_;
	vk::BufferUsageFlags usage_;
	u32 memBits_;
	std::vector<BufferSpan> updates_;
	std::vector<BufferSpan> free_;
	vpp::SubBuffer buffer_;

	GpuBuffer(rvg::Context& ctx, vk::BufferUsageFlags usage, u32 memBits);
	unsigned allocate(unsigned count);
	void free(unsigned id, unsigned count);
	bool updateDevice(unsigned typeSize, nytl::Span<const std::byte> data);
};

} // namespace detail

// TODO: add reserve, shrink mechanisms
// TODO(opt, low): we could add a FixedSizeBuffer where all allocations
// have the same size, simplifies some things and allows us to get rid of
// Span (and its size member).
template<typename T>
class Buffer {
public:
	using Type = T;

public:
	Buffer(rvg::Context& ctx, vk::BufferUsageFlags usage, u32 memBits) :
		buffer_(ctx, usage, memBits) {
	}

	// TODO: revisit/fix
	~Buffer() = default;

	Buffer(Buffer&&) = delete;
	Buffer& operator=(Buffer&&) = delete;

	unsigned allocate(unsigned count = 1) {
		auto id = buffer_.allocate(count);
		if(id == 0xFFFFFFFFu) {
			id = data_.size();
		}
		data_.resize(std::max<unsigned>(data_.size(), id + count));
		return id;
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

	bool updateDevice() {
		return buffer_.updateDevice(sizeof(T), nytl::bytes(data_));
	}

	rvg::Context& context() const { return *buffer_.context_; }
	const vpp::SubBuffer& buffer() const { return buffer_.buffer_; }
	const std::vector<T>& data() const { return data_; }

protected:
	using Span = detail::BufferSpan;
	detail::GpuBuffer buffer_;
	std::vector<T> data_;
};

} // namespace rvg
