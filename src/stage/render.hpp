#pragma once

#include <vpp/fwd.hpp>
#include <initializer_list>
#include <array>

namespace doi {

/// Shortcuts for vk::cmdBindDescriptorSets
void cmdBindGraphicsDescriptors(vk::CommandBuffer, vk::PipelineLayout,
	unsigned first, std::initializer_list<vk::DescriptorSet>,
	std::initializer_list<uint32_t> = {});
void cmdBindComputeDescriptors(vk::CommandBuffer, vk::PipelineLayout,
	unsigned first, std::initializer_list<vk::DescriptorSet>,
	std::initializer_list<uint32_t> = {});

/// Returns a PipelineColorBlendAttachmentState that disabled blending
/// but allows all components to be written.
const vk::PipelineColorBlendAttachmentState& noBlendAttachment();

/// Returns the size of mipmap level 'i' for an image that has size 'full'
vk::Extent2D mipmapSize(vk::Extent2D full, unsigned i);

// See downscale command.
struct DownscaleTarget {
	vk::Image image;
	vk::Format format;
	vk::ImageLayout layout;
	unsigned width;
	unsigned height;
	unsigned layerCount {1};

	// source scope of writing mipmap level 0
	vk::AccessFlags srcAccess;
	vk::PipelineStageFlags srcStages;
};

/// Pass that dynamically generates mipmap levels of a color image.
/// Doesn't work for image with depth format since those don't allow
/// linear filtered blitting in vulkan.
/// The given target must have at least genLevels + 1 mipLevels.
/// Undefined for genLevels == 0 (doesn't make sense).
/// Will overwrite the current contents of the first 'genLevels' levels.
/// Afterwards, the first 'genLevels' levels will be transferSrcOptimal
/// layout, with AccessBits::transferRead access in transfer stage.
/// Not optimal for non-power-of-two textures, see source code
/// for more details/example.
void downscale(vk::CommandBuffer cb, const DownscaleTarget& target,
		unsigned genLevels);

} // namespace doi
