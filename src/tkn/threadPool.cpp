#include <tkn/threadPool.hpp>
#include <dlg/dlg.hpp>
#include <iostream>

namespace tkn {
namespace {

std::atomic<ThreadPool*> threadPoolInstance = nullptr;

} // anon namespace

// The idea for using multiple queues for better scheduling (relevent for
// many short tasks) mainly from https://vorbrodt.blog/2019/02/27/advanced-thread-pool,
// see BSD-0 licensed code at https://github.com/mvorbrodt/blog

// Optimized for 'force = true' that is the usual case.
// On that path we won't use an atomic and only write it when
// called for the first time.
ThreadPool* ThreadPool::instanceIfExisting() {
	return threadPoolInstance.load();
}

ThreadPool& ThreadPool::instance() {
	static ThreadPool ini(std::thread::hardware_concurrency(), threadPoolInstance);
	return ini;
}

ThreadPool::ThreadPool(unsigned nThreads, std::atomic<ThreadPool*>& storeIn) {
	init(nThreads);
	storeIn.store(this);
}

ThreadPool::~ThreadPool() {
	destroy();
}

void ThreadPool::init(unsigned nThreads) {
	dlg_assert(workers_.empty());
	workers_ = std::vector<Worker>(nThreads);
	stop_.store(false);
	for(auto i = 0u; i < nThreads; ++i) {
		auto& worker = workers_[i];
		worker.thread = std::thread{[this, i]{ this->workerMain(i); }};
	}
}

void ThreadPool::addExplicit(Function<void()> func) {
	dlg_assert(!workers_.empty());

	// Note how we can increment lastPushed atomically but couldn't
	// easily (without locking or custom compare-swap logic) store
	// the modulo version, so we just keep incrementing it.
	// Since it's unsigned, it will eventually wrap around. When
	// numWorkers isn't a power of two (e.g. 6 or 12 are quite common
	// regarding hardware concurrency), the index will jump. But
	// since it's a heuristic anyways, this isn't a major problem.
	auto index = ++lastPushed_;
	for(auto i = 0u; i < numWorkers(); ++i) {
		// Check if we can access this workers queue
		auto& worker = workers_[(index + i) % numWorkers()];
		auto lock = std::unique_lock(worker.mutex, std::try_to_lock);
		if(lock.owns_lock()) {
			worker.tasks.push_back(std::move(func));

			lock.unlock();
			worker.cv.notify_one();
			return;
		}
	}

	// If we couldn't place the task in any not-locked queue,
	// we have to block (force lock) until we can place it in
	// the one we originally planned to.
	auto& worker = workers_[index % numWorkers()];
	auto lock = std::unique_lock(worker.mutex);
	worker.tasks.push_back(std::move(func));

	lock.unlock();
	worker.cv.notify_one();
}

void ThreadPool::workerMain(unsigned i) {
	auto& queue = workers_[i];
	auto nWorkers = numWorkers();
	while(!stop_.load()) {
		Function<void()> task;

		// Check if there is a queue from which we can pop a task.
		for(auto j = 0u; j < nWorkers; ++j) {
			auto& oq = workers_[(i + j) % nWorkers];
			auto lock = std::unique_lock(oq.mutex, std::try_to_lock);
			if(lock.owns_lock() && !oq.tasks.empty()) {
				task = std::move(oq.tasks.front());
				oq.tasks.pop_front();
				break;
			}
		}

		if(stop_.load()) {
			break;
		}

		// If we couldn't pop a task anywhere, we have to wait - either
		// for the mutex to become unlocked or for a task to be
		// placed in our own queue.
		if(!task) {
			auto lock = std::unique_lock(queue.mutex);
			queue.cv.wait(lock, [&]{
				return !queue.tasks.empty() || stop_.load();
			});

			if(stop_) {
				break;
			}
			task = std::move(queue.tasks.front());
			queue.tasks.pop_front();
		}

		// We don't want to allow tasks to terminate this worker thread.
		try {
			task();
		} catch(const std::exception& err) {
			std::cerr << "Exception in ThreadPool task: " << err.what() << "\n";
		} catch(...) {
			std::cerr << "Non-exception object thrown from ThreadPool task\n";
		}
	}
}

void ThreadPool::destroy() {
	stop_.store(true);
	for(auto& worker : workers_) {
		worker.cv.notify_one();
	}

	for(auto& worker : workers_) {
		if(worker.thread.joinable()) {
			worker.thread.join();
		}
	}

	workers_.clear();
	if(this == threadPoolInstance.load()) {
		threadPoolInstance.store(nullptr);
	}
}

} // namespace tkn
