#include "update.hpp"
#include "context.hpp"
#include <dlg/dlg.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/vk.hpp>

namespace rvg2 {

UpdateContext::UpdateContext(Context& ctx, u32 qfam) {
	init(ctx, qfam);
}

void UpdateContext::init(Context& ctx, u32 qfam) {
	dlg_assert(!valid());
	ctx_ = &ctx;

	auto& dev = ctx.device();
	uploadCb_ = dev.commandAllocator().get(qfam,
		vk::CommandPoolCreateBits::resetCommandBuffer);
	uploadSemaphore_ = vpp::Semaphore(dev);

	vk::DescriptorPoolCreateFlags dsAllocFlags {};
	if(context().descriptorIndexingFeatures()) {
		dsAllocFlags |= vk::DescriptorPoolCreateBits::updateAfterBindEXT;
	}

	dsAlloc_.init(dev, dsAllocFlags);
}

void UpdateContext::registerDeviceUpdate(DeviceObject& obj) {
	updates_.insert(&obj);
}

UpdateFlags UpdateContext::updateDevice() {
	UpdateFlags ret = UpdateFlags::none;
	for(auto* u : updates_) {
		ret |= u->updateDevice();
	}

	updates_.clear();
	return ret;
}

vk::Semaphore UpdateContext::endFrameSubmission(vk::SubmitInfo& si) {
	auto* cb = endFrameWork();
	if(!cb) {
		return {};
	}

	si.commandBufferCount = 1u;
	si.pCommandBuffers = cb;
	si.signalSemaphoreCount = 1u;
	si.pSignalSemaphores = &uploadSemaphore_.vkHandle();

	return uploadSemaphore_;
}

const vk::CommandBuffer* UpdateContext::endFrameWork() {
	if(!uploadWork_) {
		return nullptr;
	}

	vk::endCommandBuffer(uploadCb_);
	uploadWork_ = false;
	keepAliveLast_ = std::move(keepAlive_);
	return &uploadCb_.vkHandle();
}

vk::CommandBuffer UpdateContext::recordableUploadCmdBuf() {
	if(!uploadWork_) {
		uploadWork_ = true;
		vk::beginCommandBuffer(uploadCb_, {});
	}

	return uploadCb_;
}

vpp::BufferAllocator& UpdateContext::bufferAllocator() {
	return context().device().bufferAllocator();
}

vpp::DescriptorAllocator& UpdateContext::dsAllocator() {
	return dsAlloc_;
}

vpp::DeviceMemoryAllocator& UpdateContext::devMemAllocator() {
	return context().device().devMemAllocator();
}

void UpdateContext::keepAlive(vpp::SubBuffer buf) {
	keepAlive_.bufs.emplace_back(std::move(buf));
}

void UpdateContext::keepAlive(vpp::ViewableImage img) {
	keepAlive_.imgs.emplace_back(std::move(img));
}

} // namespace rvg2
