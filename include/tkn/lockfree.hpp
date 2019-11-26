#pragma once

#include <atomic>
#include <thread>
#include <cassert>

namespace tkn {

template <typename T>
class Shared {
public:
	// Most only be called from producer thread
	bool enqueue(T& val) {
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

	bool readable() const { return full_.load(std::memory_order_acquire); }
	bool writable() const { return !full_.load(std::memory_order_acquire); }

protected:
	std::atomic<bool> full_ {false};
	T value_;
};

} // namespace tkn

