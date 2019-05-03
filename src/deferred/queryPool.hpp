#pragma once

#include <vpp/fwd.hpp>
#include <vpp/resource.hpp>

// TODO: move to vpp
namespace vpp {

class QueryPool : public ResourceHandle<vk::QueryPool> {
public:
	QueryPool() = default;
	QueryPool(const Device&, const vk::QueryPoolCreateInfo&);
	QueryPool(const Device&, vk::QueryPool);
	~QueryPool();

	QueryPool(QueryPool&& rhs) noexcept { swap(*this, rhs); }
	auto& operator=(QueryPool rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}
};

} // namespace vpp
