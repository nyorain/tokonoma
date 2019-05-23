#pragma once

#include <vpp/fwd.hpp>
#include <initializer_list>
#include <array>

namespace doi {

// Shortcuts for vk::cmdBindDescriptorSets
void cmdBindGraphicsDescriptors(vk::CommandBuffer, vk::PipelineLayout,
	unsigned first, std::initializer_list<vk::DescriptorSet>,
	std::initializer_list<uint32_t> = {});
void cmdBindComputeDescriptors(vk::CommandBuffer, vk::PipelineLayout,
	unsigned first, std::initializer_list<vk::DescriptorSet>,
	std::initializer_list<uint32_t> = {});

// Returns a PipelineColorBlendAttachmentState that disabled blending
// but allows all components to be written.
const vk::PipelineColorBlendAttachmentState& noBlendAttachment();

// Returns the size of mipmap level 'i' for an image that has size 'full'
vk::Extent2D mipmapSize(vk::Extent2D full, unsigned i);

} // namespace doi
