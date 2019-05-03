#include "queryPool.hpp"
#include <vpp/vk.hpp>

namespace vpp {

QueryPool::QueryPool(const Device& dev, const vk::QueryPoolCreateInfo& info) :
	ResourceHandle(dev, vk::createQueryPool(dev, info))
{
}

QueryPool::QueryPool(const Device& dev, vk::QueryPool pool) :
	ResourceHandle(dev, pool)
{
}

QueryPool::~QueryPool() {
	if(vkHandle()) {
		vk::destroyQueryPool(device(), vkHandle());
	}
}

} // namespace vpp
