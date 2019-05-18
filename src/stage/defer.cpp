#include <stage/defer.hpp>

namespace doi {

WorkBatcher WorkBatcher::createDefault(const vpp::Device& dev) {
	return {dev, {}, {
		dev.deviceAllocator(),
		dev.deviceAllocator(),
		dev.deviceAllocator(),
		dev.bufferAllocator(),
		dev.bufferAllocator(),
		dev.bufferAllocator(),
		dev.descriptorAllocator()
	}};
}

} // namespace doi
