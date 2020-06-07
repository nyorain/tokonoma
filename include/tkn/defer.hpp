#pragma once

#include <vpp/device.hpp>
#include <vpp/handles.hpp>
#include <vpp/devMemAllocator.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>

namespace tkn {

// TODO: somewhat messy design, especially with the command buffer.
// Maybe automaticaly create it, and then submit it and wait in a 'finish'
// function?

struct WorkBatcher {
	const vpp::Device& dev;
	vk::CommandBuffer cb;

	struct {
		vpp::DescriptorAllocator& ds;

		vpp::DeviceMemoryAllocator memDevice;
		vpp::DeviceMemoryAllocator memHost;
		vpp::DeviceMemoryAllocator memStage;

		vpp::BufferAllocator bufDevice;
		vpp::BufferAllocator bufHost;
		vpp::BufferAllocator bufStage;
	} alloc;

	/// Initializes all allocators with the devices defaults.
	/// CommandBuffer will be left empty.
	explicit WorkBatcher(const vpp::Device& dev);

	/// Merges all allocators with the device's default ones.
	/// Expects all resource allocations to be finished.
	~WorkBatcher();
};

} // namespace tkn
