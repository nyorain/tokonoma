#pragma once

#include <vpp/device.hpp>
#include <vpp/handles.hpp>

namespace doi {

struct WorkBatcher {
	const vpp::Device& dev;
	vk::CommandBuffer cb;

	struct {
		vpp::DeviceMemoryAllocator& memDevice;
		vpp::DeviceMemoryAllocator& memHost;
		vpp::DeviceMemoryAllocator& memStage;

		vpp::BufferAllocator& bufDevice;
		vpp::BufferAllocator& bufHost;
		vpp::BufferAllocator& bufStage;

		vpp::DescriptorAllocator& ds;
	} alloc;

	/// Initializes all allocators with the devices defaults.
	/// CommandBuffer will be left empty.
	WorkBatcher(const vpp::Device& dev);

	// TODO: util
	// void finish(); // automatically call in destructor?
};

} // namespace doi
