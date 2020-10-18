#pragma once

#include "common.hpp"
#include <vpp/handles.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vector>
#include <unordered_set>

namespace rvg2 {

class DeviceObject;

// NOTE WIP
// high level multithread sketch
class UpdateContext {
public:
	UpdateContext() = default;
	UpdateContext(Context& ctx, u32 qfam);

	UpdateContext(UpdateContext&&) = delete;
	UpdateContext& operator=(UpdateContext&&) = delete;

	void init(Context& ctx, u32 qfam);

	UpdateFlags updateDevice();
	vk::Semaphore endFrameSubmission(vk::SubmitInfo&);
	const vk::CommandBuffer* endFrameWork();

	vpp::BufferAllocator& bufferAllocator();
	vpp::DescriptorAllocator& dsAllocator();
	vpp::DeviceMemoryAllocator& devMemAllocator();

	vk::CommandBuffer recordableUploadCmdBuf();
	void keepAlive(vpp::SubBuffer);
	void keepAlive(vpp::ViewableImage);

	void registerDeviceUpdate(DeviceObject&);
	void deviceObjectDestroyed(DeviceObject&);

	bool valid() const { return ctx_; }
	auto& context() const { return *ctx_; }

private:
	Context* ctx_ {};

	vpp::DescriptorAllocator dsAlloc_ {};

	vpp::CommandBuffer uploadCb_;
	vpp::Semaphore uploadSemaphore_;
	bool uploadWork_ {}; // whether there is work

	struct KeepAlive {
		std::vector<vpp::SubBuffer> bufs;
		std::vector<vpp::ViewableImage> imgs;
	};

	KeepAlive keepAlive_;
	KeepAlive keepAliveLast_;

	// resources registered for updateDevice
	std::unordered_set<DeviceObject*> updates_;
};

/// Base for classes that manage resource on the gpu.
/// Since they can't just change the gpu data, they will register
class DeviceObject {
public:
	DeviceObject() = default;
	DeviceObject(UpdateContext& ctx) : context_(&ctx) {}

	virtual ~DeviceObject() = default;
	virtual UpdateFlags updateDevice() = 0;

	/// Returns whether this object is valid.
	/// It is not valid when it was default constructed or moved from.
	/// Performing any operations on invalid objects triggers
	/// undefined behavior.
	bool valid() const { return context_; }

	/// Returns the associated update context.
	/// Error to call this if the object isn't valid.
	UpdateContext& updateContext() const { return *context_; }

	/// Returns the associated context.
	/// Error to call this if the object isn't valid.
	Context& context() const { return context_->context(); }

protected:
	UpdateContext* context_ {};

	void registerDeviceUpdate() {
		context_->registerDeviceUpdate(*this);
	}
};

} // namespace rvg2
