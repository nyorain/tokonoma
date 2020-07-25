#pragma once

#include <tkn/function.hpp>
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
	// ThreadPool is threadsafe, any thread can queue new tasks.
	static ThreadPool& instance();

	// Returns the instance only if it is already existing.
	static ThreadPool* instanceIfExisting();

public:
	ThreadPool() = default;
	explicit ThreadPool(unsigned nThreads) { init(nThreads); }

	ThreadPool(const ThreadPool& rhs) = delete;
	ThreadPool& operator =(ThreadPool& rhs) = delete;

	~ThreadPool();

	void init(unsigned nThreads);
	void addExplicit(Function<void()> func);
	void add(Function<void()> func) { addExplicit(std::move(func)); }
	unsigned numWorkers() const { return workers_.size(); }

	template<typename F, typename... Args>
	void add(F&& func, Args&&... args) {
		addExplicit([
				func = std::forward<F>(func),
				args = std::make_tuple(std::forward<Args>(args)...)]{
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
		addExplicit([
				func = std::forward<F>(func),
				args = std::forward_as_tuple(std::forward<Args>(args)...),
				promise = std::move(promise)]() mutable {
			try {
				if constexpr(std::is_void_v<R>) {
					std::apply(std::forward<F>(func), std::move(args));
					promise.set_value();
				} else {
					promise.set_value(std::apply(std::forward<F>(func), std::move(args)));
				}
			} catch(...) {
				promise.set_exception(std::current_exception());
			}
		});

		return future;
	}

	template<typename F, typename... Args>
	auto addPromised(F&& func) {
		using R = decltype(std::forward<F>(func)());
		std::promise<R> promise;
		auto future = promise.get_future();
		addExplicit([
				func = std::forward<F>(func),
				promise = std::move(promise)]() mutable {
			try {
				if constexpr(std::is_void_v<R>) {
					std::forward<F>(func)();
					promise.set_value();
				} else {
					promise.set_value(std::forward<F>(func)());
				}
			} catch(...) {
				promise.set_exception(std::current_exception());
			}
		});

		return future;
	}

	// Signals all workers to quit and wait until all pending tasks are
	// completed. Automatically called from destructor.
	void destroy();

protected:
	ThreadPool(unsigned nThreads, std::atomic<ThreadPool*>& storeIn);
	void workerMain(unsigned i);

protected:
	struct Worker {
		std::thread thread;
		std::deque<Function<void()>> tasks;
		std::condition_variable cv;
		std::condition_variable cvWait;
		std::mutex mutex;
	};

	std::vector<Worker> workers_;
	std::atomic<bool> stop_ {false}; // Set to true in destructor.
	std::atomic<unsigned> lastPushed_ {0u}; // The queue we last pushed to.
};

} // namespace tkn

