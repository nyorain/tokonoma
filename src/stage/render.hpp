// WIP: rendering utilities

#pragma once

#include <vpp/fwd.hpp>
#include <initializer_list>

namespace doi {

// Shortcuts for vk::cmdBindDescriptorSets
void cmdBindGraphicsDescriptors(vk::CommandBuffer, vk::PipelineLayout,
	std::initializer_list<vk::DescriptorSet>,
	std::initializer_list<vk::DeviceSize> = {});
void cmdBindComputeDescriptors(vk::CommandBuffer, vk::PipelineLayout,
	std::initializer_list<vk::DescriptorSet>,
	std::initializer_list<vk::DeviceSize> = {});

// Returns a PipelineColorBlendAttachmentState that disabled blending
// but allows all components to be written.
const vk::PipelineColorBlendAttachmentState& noBlendAttachment();

// Returns the size of mipmap level 'i' for an image that has size 'full'
vk::Extent2D mipmapSize(const vk::Extent2D& full, unsigned i);

} // namespace doi
