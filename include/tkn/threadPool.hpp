#pragma once

#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <condition_variable>
#include <vector>
#include <future>
#include <type_traits>

namespace tkn {

// Generic efficient ThreadPool implementation. Not lockfree but tries
// to minimize the number of locks.
class ThreadPool {
public:
	ThreadPool() = default;
	explicit ThreadPool(unsigned nThreads);

	ThreadPool(const ThreadPool& rhs) = delete;
	ThreadPool& operator =(ThreadPool& rhs) = delete;

	~ThreadPool();

	void add(std::function<void()> func);
	unsigned numWorkers() const { return workers_.size(); }

	template<typename F, typename... Args>
	void add(F&& func, Args&&... args) {
		add([func = std::forward<F>(func), args = std::make_tuple(std::forward<Args>(args)...)]{
			std::apply(func, std::move(args));
		});
	}

	// Adds a task with a promise, returns the associated future.
	// This is useful to get the return value of a function when it has
	// finished, but also for checking for thrown exceptions or waiting
	// for a queues function to complete.
	template<typename F, typename... Args>
	auto addPromised(F&& func, Args&&... args) {
		using R = decltype(func(std::forward<Args>(args)...));
		std::promise<R> promise;
		auto future = promise.get_future();
		add([func = std::forward<F>(func),
				args = std::make_tuple(std::forward<Args>(args)...),
				promise = std::move(promise)]{
			try {
				if constexpr(std::is_void_v<R>) {
					std::apply(func, std::move(args));
					promise.set_value();
				} else {
					promise.set_value(std::apply(func, std::move(args)));
				}
			} catch(...) {
				promise.set_exception(std::current_exception());
			}
		});

		return future;
	}

protected:
	void workerMain(unsigned i);

protected:
	struct Worker {
		std::deque<std::function<void()>> tasks;
		std::condition_variable cv;
		std::condition_variable cvWait;
		std::mutex mutex;
		std::thread thread;
	};

	std::vector<Worker> workers_;
	std::atomic<bool> stop_ {false}; // Set to true in destructor.
	std::atomic<unsigned> lastPushed_ {0u}; // The queue we last pushed to.
};

} // namespace tkn

