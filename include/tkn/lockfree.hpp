#pragma once

#include <atomic>

namespace tkn {

// Basically an optimized version of a single-capacity ring buffer.
// Extremely useful for lockfree single-value communication, where
// one thread can send the other thread values. Can be seen as a
// lock-free communication pipe with a buffer size of 1.
template <typename T>
class Shared {
public:
	// Most only be called from producer thread
	bool enqueue(T val) {
		if(!writable()) {
			return false;
		}

		value_ = std::move(val);
		full_.store(true, std::memory_order_release);
		return true;
	}

	// Most only be called from consumer thread
	bool dequeue(T& val) {
		if(!readable()) {
			return false;
		}

		val = std::move(value_);
		full_.store(false, std::memory_order_release);
		return true;
	}

	// Usually not needed, prefer to direclty use enqueue and dequeue and
	// simply check the return value.
	bool readable() const { return full_.load(std::memory_order_acquire); }
	bool writable() const { return !full_.load(std::memory_order_acquire); }

protected:
	std::atomic<bool> full_ {false};
	T value_;
};

} // namespace tkn

