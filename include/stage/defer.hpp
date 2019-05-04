#pragma once

#include <vpp/device.hpp>
#include <vpp/commandBuffer.hpp>

// WIP: deferred initialization utility

namespace doi {

struct WorkBatcher {
	const vpp::Device& dev;
	vk::CommandBuffer cb;
};

template<typename T>
class Initializer {
public:
	using InitData = typename T::InitData;

public:
	template<typename... Args>
	Initializer(const WorkBatcher& batcher, Args&&... args) :
		resource_(batcher, initData_, std::forward<Args>(args)...) {
	}

	void alloc(const WorkBatcher& batcher) {
		resource_.initAlloc(batcher, initData_);
	}

	T finish(const WorkBatcher& batcher) { // pass work batcher?
		resource_.initFinish(batcher, initData_);
		return std::move(resource_);
	}

protected:
	InitData initData_ {};
	T resource_;
};

// TODO: RAII initializer that takes reference to target objects
// and automatically moves in destructor (when going out of scope)

} // namespace doi
