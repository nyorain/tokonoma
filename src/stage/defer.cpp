#include <stage/defer.hpp>

namespace doi {

WorkBatcher WorkBatcher::createDefault(const vpp::Device& dev) {
	return {dev, {}, {
		dev.devMemAllocator(),
		dev.devMemAllocator(),
		dev.devMemAllocator(),
		dev.bufferAllocator(),
		dev.bufferAllocator(),
		dev.bufferAllocator(),
		dev.descriptorAllocator()
	}};
}

} // namespace doi
