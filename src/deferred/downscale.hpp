#pragma once

#include <stage/render.hpp>
#include <stage/types.hpp>

#include <vpp/vk.hpp>
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <nytl/vecOps.hpp>
#include <dlg/dlg.hpp>

struct DownscaleTarget {
	vk::Image image;
	vk::Format format;
	vk::ImageLayout layout;
	vk::Extent2D size;
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
void downscale(vk::CommandBuffer cb, const DownscaleTarget& target,
		unsigned genLevels) {
	dlg_assert(cb && target.image && target.layerCount);
	dlg_assert(genLevels > 0);

	// transition mipmap 0 to layout transferSrcOptimal
	// transition other levels to layout transferDstOptimal
	vk::ImageMemoryBarrier barrier0;
	barrier0.image = target.image;
	barrier0.subresourceRange.layerCount = target.layerCount;
	barrier0.subresourceRange.levelCount = 1;
	barrier0.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	barrier0.oldLayout = target.layout;
	barrier0.srcAccessMask = target.srcAccess;
	barrier0.newLayout = vk::ImageLayout::transferSrcOptimal;
	barrier0.dstAccessMask = vk::AccessBits::transferRead;

	vk::ImageMemoryBarrier barrierRest;
	barrierRest.image = target.image;
	barrierRest.subresourceRange.baseMipLevel = 1;
	barrierRest.subresourceRange.layerCount = target.layerCount;
	barrierRest.subresourceRange.levelCount = genLevels;
	barrierRest.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	barrierRest.oldLayout = vk::ImageLayout::undefined; // overwrite
	barrierRest.srcAccessMask = {};
	barrierRest.newLayout = vk::ImageLayout::transferDstOptimal;
	barrierRest.dstAccessMask = vk::AccessBits::transferWrite;

	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::topOfPipe | target.srcStages,
		vk::PipelineStageBits::transfer,
		{}, {}, {}, {{barrier0, barrierRest}});

	for(auto i = 1u; i < genLevels; ++i) {
		// std::max needed for end offsets when the texture is not
		// quadratic: then we would get 0 there although the mipmap
		// still has size 1
		vk::ImageBlit blit;
		blit.srcSubresource.layerCount = target.layerCount;
		blit.srcSubresource.mipLevel = i - 1;
		blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.srcOffsets[1].x = std::max(target.size.width >> (i - 1), 1u);
		blit.srcOffsets[1].y = std::max(target.size.height >> (i - 1), 1u);
		blit.srcOffsets[1].z = 1u;

		blit.dstSubresource.layerCount = target.layerCount;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.dstOffsets[1].x = std::max(target.size.width >> i, 1u);
		blit.dstOffsets[1].y = std::max(target.size.height >> i, 1u);
		blit.dstOffsets[1].z = 1u;

		vk::cmdBlitImage(cb, target.image, vk::ImageLayout::transferSrcOptimal,
			target.image, vk::ImageLayout::transferDstOptimal, {{blit}},
			vk::Filter::linear);

		// change layout of current mip level to transferSrc for next mip level
		vk::ImageMemoryBarrier barrieri;
		barrieri.image = target.image;
		barrieri.subresourceRange.baseMipLevel = i;
		barrieri.subresourceRange.layerCount = 1;
		barrieri.subresourceRange.levelCount = target.layerCount;
		barrieri.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		barrieri.oldLayout = vk::ImageLayout::transferDstOptimal;
		barrieri.srcAccessMask = vk::AccessBits::transferWrite;
		barrieri.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrieri.dstAccessMask = vk::AccessBits::transferRead;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrieri}});
	}

	// NOTE: that's what a barrier transitioning to use after this would
	// look like.
	// vk::ImageMemoryBarrier barrier;
	// barrier.image = target.image;
	// barrier.subresourceRange.layerCount = target.layerCount;
	// barrier.subresourceRange.levelCount = genLevels + 1;
	// barrier.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	// barrier.oldLayout = vk::ImageLayout::transferSrcOptimal;
	// barrier.srcAccessMask = vk::AccessBits::transferRead;
	// barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	// barrier.dstAccessMask = vk::AccessBits::shaderRead;
	// vk::cmdPipelineBarrier(cb,
	// 	vk::PipelineStageBits::transfer,
	// 	vk::PipelineStageBits::allCommands,
	// 	{}, {}, {}, {{barrier}});
}
