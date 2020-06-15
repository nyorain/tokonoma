#pragma once

#include <nytl/span.hpp>
#include <memory>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <type_traits>

// NOTE: not tested yet.

template<typename T>
void copy(T* dst, const T* src, size_t count) {
	if constexpr(std::is_trivially_copyable_v<T>) {
		std::memcpy(dst, src, count * sizeof(T));
	} else {
		std::move(src, src + count, dst);
	}
}

template<typename T>
void construct(T* dst, size_t count) {
	if constexpr(std::is_trivially_constructible_v<T>) {
		std::memset(dst, 0x0, count * sizeof(T));
	} else {
		for(auto i = 0u; i < count; ++i) dst[i] = {};
	}
}

// Single-producer, single-consumer ringbuffer. Size of it is given
// at construction.
// The indexing is explained here:
// https://www.snellman.net/blog/archive/2016-12-13-ring-buffers/
//
// - T: type of data to store
// - OneThread: whether there is (for *all* time) just one writer and
//   reader thread. Even if this is false, calling write or read
//   respectively from multiple threads at the same time is undefined.
//   But when this is true, 'read' and 'write' must respectively *always*
//   be called by the same thread. Using multiple threads can be useful
//   to allow the writer or reader thread to change (but that must be
//   externally synchronized then).
template<typename T, bool OneThread = false>
class RingBuffer {
public:
	using Size = std::uint64_t;

	static constexpr auto rLoad = OneThread ?
		std::memory_order_relaxed : std::memory_order_acquire;
	static constexpr auto rStore = OneThread ?
		std::memory_order_relaxed : std::memory_order_release;

public:
	RingBuffer() = default;
	RingBuffer(Size capacity) : capacity_(capacity) {
		// The capacity must be a power of two so that natural unsigned
		// integer wrapping on write_ and read_ works.
		assert(capacity % 2 == 0);
		data_ = std::make_unique<std::byte[]>(capacity);
	}

	Size write(nytl::Span<const T> span) {
		auto wr = write_.load(rLoad);
		auto rd = read_.load(rLoad);

		Size count = std::min(Size(span.size()), capacity() - (wr - rd));
		Size first = std::min(capacity() - mask(wr), count);
		Size second = count - first;

		copy(data_.get() + mask(wr), span.data(), first);
		copy(data_.get(), span.data() + first, second);

		// Always use release here so that if this advance is visible
		// to the reader thread, the written data will be as well.
		write_.store(wr + count, std::memory_order_release);
		return count;
	}

	Size read(nytl::Span<T> span) {
		// Always use acquire here so that if the advance of the
		// write is visible (done with 'release' there), the written data
		// will be visible as well.
		auto wr = write_.load(std::memory_order_acquire);
		auto rd = read_.load(rLoad);

		Size count = std::min(Size(span.size()), wr - rd);
		Size first = std::min(capacity() - mask(rd), count);
		Size second = count - first;

		copy(span.data(), data_.get() + mask(read_), first);
		copy(span.data() + first, data_.get(), second);

		read_.store(rd + count, rStore);
		return count;
	}

	Size writeEmpty(Size count) {
		auto wr = write_.load(rLoad);
		auto rd = read_.load(rLoad);

		count = std::min(count, capacity() - (wr - rd));
		Size first = std::min(capacity() - mask(wr), count);
		Size second = count - first;

		construct(data_.get() + mask(wr), first);
		construct(data_.get(), second);

		// Always use release here so that if this advance is visible
		// to the reader thread, the written data will be as well.
		write_.store(wr + count, std::memory_order_release);
		return count;
	}

	// Keep in mind that these value may immediately out of date.
	// But: When called from a reader thread, the number of readable bytes
	// will be at least 'readable()' and when called from a writer thread,
	// the number of writable bytes will be at least 'writable()'.
	// Calling the respective functions from the opposite thread types
	// gives almost no guarantees and is therefore pretty much never useful.
	Size readable() const { return write_.load(rLoad) - read_.load(rLoad); }
	Size writable() const { return capacity() - readable(); }

	bool empty() const { return read_.load(rLoad) == write_.load(rLoad); }
	bool full() const { return readable() == capacity(); }
	Size capacity() const { return capacity_; }
	Size mask(Size in) const { return in & (capacity_ - 1); }

protected:
	Size capacity_ {};
	std::atomic<Size> read_ {};
	std::atomic<Size> write_ {};
	std::unique_ptr<T[]> data_;
};

