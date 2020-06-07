#include <tkn/defer.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

WorkBatcher::WorkBatcher(const vpp::Device& xdev) : dev(xdev), cb(),
		alloc {xdev.descriptorAllocator(), {}, {}, {}, {}, {}, {}} {

	alloc.memDevice.init(dev);
	alloc.memHost.init(dev);
	alloc.memStage.init(dev);

	alloc.memDevice.restrict(dev.deviceMemoryTypes());
	alloc.memHost.restrict(dev.hostMemoryTypes());
	alloc.memStage.restrict(dev.hostMemoryTypes());

	alloc.bufDevice.init(alloc.memDevice);
	alloc.bufStage.init(alloc.memStage);
	alloc.bufHost.init(alloc.memHost);
}

WorkBatcher::~WorkBatcher() {
	auto b1 = dev.bufferAllocator().tryMergeBuffers(std::move(alloc.bufStage));
	auto b2 = dev.bufferAllocator().tryMergeBuffers(std::move(alloc.bufHost));
	auto b3 = dev.bufferAllocator().tryMergeBuffers(std::move(alloc.bufDevice));
	dlg_assert(b1 && b2 && b3);

	auto m1 = dev.devMemAllocator().tryMergeMemories(std::move(alloc.memStage));
	auto m2 = dev.devMemAllocator().tryMergeMemories(std::move(alloc.memHost));
	auto m3 = dev.devMemAllocator().tryMergeMemories(std::move(alloc.memDevice));
	dlg_assert(m1 && m2 && m3);
}

} // namespace tkn
