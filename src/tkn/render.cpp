#include <tkn/render.hpp>
#include <tkn/formats.hpp>
#include <vpp/vk.hpp>

#include <vpp/vk.hpp>
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

// Shortcuts for vk::cmdBindDescriptorSets
void cmdBindGraphicsDescriptors(vk::CommandBuffer cb, vk::PipelineLayout pl,
		unsigned first, std::initializer_list<vk::DescriptorSet> ds,
		std::initializer_list<uint32_t> off) {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, first, {ds}, {off});
}

void cmdBindComputeDescriptors(vk::CommandBuffer cb, vk::PipelineLayout pl,
		unsigned first, std::initializer_list<vk::DescriptorSet> ds,
		std::initializer_list<uint32_t> off) {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pl, first, {ds}, {off});
}

void cmdBindVertexBuffers(vk::CommandBuffer cb,
		nytl::Span<const vpp::BufferSpan> bufs, unsigned first) {
	constexpr auto maxCount = 256;
	dlg_assert(bufs.size() < maxCount);

	std::array<vk::Buffer, maxCount> handles;
	std::array<vk::DeviceSize, maxCount> offsets;

	for(auto i = 0u; i < bufs.size(); ++i) {
		handles[i] = bufs[i].buffer();
		offsets[i] = bufs[i].offset();
	}

	vk::cmdBindVertexBuffers(cb, first, bufs.size(),
		*handles.data(), *offsets.data());
}

void cmdCopyBuffer(vk::CommandBuffer cb, vpp::BufferSpan src,
		vpp::BufferSpan dst) {
	dlg_assert(src.size() == dst.size());

	vk::BufferCopy copy;
	copy.srcOffset = src.offset();
	copy.dstOffset = dst.offset();
	copy.size = src.size();
	vk::cmdCopyBuffer(cb, src.buffer(), dst.buffer(), {{copy}});
}

// Returns a PipelineColorBlendAttachmentState that disabled blending
// but allows all components to be written.
const vk::PipelineColorBlendAttachmentState& noBlendAttachment() {
	static constexpr vk::PipelineColorBlendAttachmentState state = {
		false, {}, {}, {}, {}, {}, {},
		vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a,
	};
	return state;
}

const vk::PipelineColorBlendAttachmentState& defaultBlendAttachment() {
	static constexpr vk::PipelineColorBlendAttachmentState state = {
		true, // blending enabled
		// color
		vk::BlendFactor::srcAlpha, // src
		vk::BlendFactor::oneMinusSrcAlpha, // dst
		vk::BlendOp::add,
		// alpha
		// Rationale behind using the dst alpha is that there
		// is no use in storing the src alpha somewhere, as
		// we've already processed it via the color blending above.
		vk::BlendFactor::zero, // src
		vk::BlendFactor::one, // dst
		vk::BlendOp::add,
		// color write mask
		vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a,
	};
	return state;
}

const vk::PipelineColorBlendAttachmentState& disableBlendAttachment() {
	static constexpr vk::PipelineColorBlendAttachmentState state = {};
	return state;
}

// Returns the size of mipmap level 'i' for an image that has size 'full'
vk::Extent2D mipmapSize(vk::Extent2D full, unsigned i) {
	vk::Extent2D ret;
	ret.width = std::max(full.width >> i, 1u);
	ret.height = std::max(full.height >> i, 1u);
	return ret;
}

void downscale(vk::CommandBuffer cb, const DownscaleTarget& target,
		unsigned genLevels, const SyncScope* dst) {
	dlg_assert(cb && target.image && target.layerCount);
	dlg_assert(genLevels > 0);

	// transition base mipmap to layout transferSrcOptimal
	// transition higher levels to layout transferDstOptimal
	vk::ImageMemoryBarrier barrier0;
	barrier0.image = target.image;
	barrier0.subresourceRange.layerCount = target.layerCount;
	barrier0.subresourceRange.baseMipLevel = target.baseLevel;
	barrier0.subresourceRange.levelCount = 1;
	barrier0.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	barrier0.oldLayout = target.srcScope.layout;
	barrier0.srcAccessMask = target.srcScope.access;
	barrier0.newLayout = vk::ImageLayout::transferSrcOptimal;
	barrier0.dstAccessMask = vk::AccessBits::transferRead;

	vk::ImageMemoryBarrier barrierRest;
	barrierRest.image = target.image;
	barrierRest.subresourceRange.baseMipLevel = target.baseLevel + 1;
	barrierRest.subresourceRange.layerCount = target.layerCount;
	barrierRest.subresourceRange.levelCount = genLevels;
	barrierRest.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	barrierRest.oldLayout = vk::ImageLayout::undefined; // overwrite
	barrierRest.srcAccessMask = {};
	barrierRest.newLayout = vk::ImageLayout::transferDstOptimal;
	barrierRest.dstAccessMask = vk::AccessBits::transferWrite;

	vk::cmdPipelineBarrier(cb,
		// topOfPipe for the mip levels
		vk::PipelineStageBits::topOfPipe | target.srcScope.stages,
		vk::PipelineStageBits::transfer,
		{}, {}, {}, {{barrier0, barrierRest}});

	for(auto i = target.baseLevel; i < target.baseLevel + genLevels; ++i) {
		// std::max needed for end offsets when the texture is not
		// quadratic: then we would get 0 there although the mipmap
		// still has size 1
		vk::ImageBlit blit;
		blit.srcSubresource.layerCount = target.layerCount;
		blit.srcSubresource.mipLevel = i;
		blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
		// NOTE: the '& ~1u' causes the size to be the largest even number
		// below the real mipmap size. Kind of important for non-power-of-two
		// textures since there we e.g. have mip level size 3x3 and then
		// 1x1 on the next smaller one. In that case the 1x1 would simply
		// contain the center texel from the 3x3 mip which is not what
		// we want (think about getting the average texture value via
		// downsampling e.g. for luminance; we simply dicard 8/9 of the
		// texture). With this we will at least scale 2x2 from the first
		// mipmap level to the next one and so only discard the 5 right/bottom
		// outer pixels. Still somewhat bad though!
		blit.srcOffsets[1].x = std::max((target.extent.width >> i) & ~1u, 1u);
		blit.srcOffsets[1].y = std::max((target.extent.height >> i) & ~1u, 1u);
		blit.srcOffsets[1].z = std::max((target.extent.depth >> i) & ~1u, 1u);

		blit.dstSubresource.layerCount = target.layerCount;
		blit.dstSubresource.mipLevel = i + 1;
		blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.dstOffsets[1].x = std::max(target.extent.width >> (i + 1), 1u);
		blit.dstOffsets[1].y = std::max(target.extent.height >> (i + 1), 1u);
		blit.dstOffsets[1].z = std::max(target.extent.depth >> (i + 1), 1u);

		vk::cmdBlitImage(cb, target.image, vk::ImageLayout::transferSrcOptimal,
			target.image, vk::ImageLayout::transferDstOptimal, {{blit}},
			vk::Filter::linear);

		// change layout of dst mip level to transferSrc for next mip level
		// we even do it for the last level so that we have consistent src
		// scopes for all levels.
		vk::ImageMemoryBarrier barrieri;
		barrieri.image = target.image;
		barrieri.subresourceRange.layerCount = target.layerCount;
		barrieri.subresourceRange.baseMipLevel = i + 1;
		barrieri.subresourceRange.levelCount = 1u;
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

	if(dst) {
		vk::ImageMemoryBarrier barrier;
		barrier.image = target.image;
		barrier.subresourceRange.baseMipLevel = target.baseLevel;
		barrier.subresourceRange.layerCount = target.layerCount;
		barrier.subresourceRange.levelCount = genLevels + 1;
		barrier.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		barrier.oldLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.srcAccessMask = vk::AccessBits::transferRead;
		barrier.newLayout = dst->layout;
		barrier.dstAccessMask = dst->access;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer, dst->stages,
			{}, {}, {}, {{barrier}});
	}
}

void downscale(const vpp::Device& dev, vk::CommandBuffer cb,
		const DownscaleTarget& target, unsigned genLevels,
		const SyncScope* dst) {
	vpp::DebugLabel(dev, cb, "tkn::downscale");
	downscale(cb, target, genLevels, dst);
}

void downscale(const vpp::CommandBuffer& cb,
		const DownscaleTarget& target, unsigned genLevels,
		const SyncScope* dst) {
	downscale(cb.device(), cb, target, genLevels, dst);
}

void barrier(vk::CommandBuffer cb, nytl::Span<const ImageBarrier> barriers) {
	dlg_assert(!barriers.empty());

	std::vector<vk::ImageMemoryBarrier> vkBarriers;
	vkBarriers.reserve(barriers.size());
	vk::PipelineStageFlags srcStages = {};
	vk::PipelineStageFlags dstStages = {};
	for(auto& b : barriers) {
		if(!b.image) { // empty barriers allowed
			continue;
		}

		srcStages |= b.src.stages;
		dstStages |= b.dst.stages;

		auto& barrier = vkBarriers.emplace_back();
		barrier.image = b.image;
		barrier.srcAccessMask = b.src.access;
		barrier.oldLayout = b.src.layout;
		barrier.dstAccessMask = b.dst.access;
		barrier.newLayout = b.dst.layout;
		barrier.subresourceRange = b.subres;
	}

	if(vkBarriers.empty()) {
		return;
	}

	vk::cmdPipelineBarrier(cb, srcStages, dstStages, {}, {}, {}, vkBarriers);
}

void barrier(vk::CommandBuffer cb, vk::Image image, SyncScope src,
		SyncScope dst, vk::ImageSubresourceRange subres) {
	vk::ImageMemoryBarrier barrier;
	barrier.image = image;
	barrier.srcAccessMask = src.access;
	barrier.oldLayout = src.layout;
	barrier.dstAccessMask = dst.access;
	barrier.newLayout = dst.layout;
	barrier.subresourceRange = subres;
	vk::cmdPipelineBarrier(cb, src.stages, dst.stages, {}, {}, {}, {{barrier}});
}

bool operator==(SyncScope a, SyncScope b) {
	return a.stages == b.stages &&
		a.layout == b.layout &&
		a.access == b.access;
}

bool operator!=(SyncScope a, SyncScope b) {
	return a.stages != b.stages ||
		a.layout != b.layout ||
		a.access != b.access;
}

SyncScope& operator|=(SyncScope& a, SyncScope b) {
	if(a.layout == vk::ImageLayout::undefined) {
		a.layout = b.layout;
	} else {
		dlg_assert(b.layout == vk::ImageLayout::undefined ||
			a.layout == b.layout);
	}

	a.stages |= b.stages;
	a.access |= b.access;
	return a;
}

SyncScope operator|(SyncScope a, SyncScope b) {
	return a |= b;
}

vk::SamplerCreateInfo linearSamplerInfo(vk::SamplerAddressMode mode) {
	vk::SamplerCreateInfo sci;
	sci.addressModeU = mode;
	sci.addressModeV = mode;
	sci.addressModeW = mode;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.mipmapMode = vk::SamplerMipmapMode::linear;
	sci.minLod = 0.0;
	sci.maxLod = 100.0;
	sci.anisotropyEnable = false;
	return sci;
}

vk::SamplerCreateInfo nearestSamplerInfo(vk::SamplerAddressMode mode) {
	vk::SamplerCreateInfo sci;
	sci.addressModeU = mode;
	sci.addressModeV = mode;
	sci.addressModeW = mode;
	sci.magFilter = vk::Filter::nearest;
	sci.minFilter = vk::Filter::nearest;
	sci.mipmapMode = vk::SamplerMipmapMode::nearest;
	sci.minLod = 0.0;
	sci.maxLod = 100.0;
	sci.anisotropyEnable = false;
	return sci;
}

RenderPassInfo renderPassInfo(nytl::Span<const vk::Format> formats,
		nytl::Span<const nytl::Span<const unsigned>> passes) {
	RenderPassInfo rpi;
	for(auto f : formats) {
		auto& a = rpi.attachments.emplace_back();
		a.format = f;
		a.initialLayout = vk::ImageLayout::undefined;
		a.finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		a.loadOp = vk::AttachmentLoadOp::clear;
		a.storeOp = vk::AttachmentStoreOp::store;
		a.samples = vk::SampleCountBits::e1;
	}

	for(auto pass : passes) {
		auto& subpass = rpi.subpasses.emplace_back();
		auto& colorRefs = rpi.colorRefs.emplace_back();
		subpass.pipelineBindPoint = vk::PipelineBindPoint::graphics;

		bool depth = false;
		for(auto id : pass) {
			dlg_assert(id < rpi.attachments.size());
			auto format = formats[id];
			if(isDepthFormat(format)) {
				dlg_assertm(!depth, "More than one depth attachment");
				depth = true;
				auto& ref = rpi.depthRefs.emplace_back();
				ref.attachment = id;
				ref.layout = vk::ImageLayout::depthStencilAttachmentOptimal;
				subpass.pDepthStencilAttachment = &ref;
			} else {
				auto& ref = colorRefs.emplace_back();
				ref.attachment = id;
				ref.layout = vk::ImageLayout::colorAttachmentOptimal;
			}
		}

		subpass.pColorAttachments = colorRefs.data();
		subpass.colorAttachmentCount = colorRefs.size();
	}

	return rpi;
}

} // namespace tkn
