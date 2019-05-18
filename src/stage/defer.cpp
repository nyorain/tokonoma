#include <stage/defer.hpp>

namespace doi {

WorkBatcher::WorkBatcher(const vpp::Device& dev) : dev(dev),
		alloc{
			dev.deviceAllocator(),
			dev.deviceAllocator(),
			dev.deviceAllocator(),
			dev.bufferAllocator(),
			dev.bufferAllocator(),
			dev.bufferAllocator(),
			dev.descriptorAllocator()} {
}

} // namespace doi
