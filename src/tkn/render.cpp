#include <tkn/render.hpp>
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
		vk::BlendFactor::one,
		vk::BlendFactor::zero,
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

	// transition mipmap 0 to layout transferSrcOptimal
	// transition other levels to layout transferDstOptimal
	vk::ImageMemoryBarrier barrier0;
	barrier0.image = target.image;
	barrier0.subresourceRange.layerCount = target.layerCount;
	barrier0.subresourceRange.levelCount = 1;
	barrier0.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	barrier0.oldLayout = target.srcScope.layout;
	barrier0.srcAccessMask = target.srcScope.access;
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
		// topOfPipe for the mip levels
		vk::PipelineStageBits::topOfPipe | target.srcScope.stages,
		vk::PipelineStageBits::transfer,
		{}, {}, {}, {{barrier0, barrierRest}});

	for(auto i = 1u; i < genLevels + 1; ++i) {
		// std::max needed for end offsets when the texture is not
		// quadratic: then we would get 0 there although the mipmap
		// still has size 1
		vk::ImageBlit blit;
		blit.srcSubresource.layerCount = target.layerCount;
		blit.srcSubresource.mipLevel = i - 1;
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
		blit.srcOffsets[1].x = std::max((target.extent.width >> (i - 1)) & ~1u, 1u);
		blit.srcOffsets[1].y = std::max((target.extent.height >> (i - 1)) & ~1u, 1u);
		blit.srcOffsets[1].z = std::max((target.extent.depth >> (i - 1)) & ~1u, 1u);

		blit.dstSubresource.layerCount = target.layerCount;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.dstOffsets[1].x = std::max(target.extent.width >> i, 1u);
		blit.dstOffsets[1].y = std::max(target.extent.height >> i, 1u);
		blit.dstOffsets[1].z = std::max(target.extent.depth >> i, 1u);

		vk::cmdBlitImage(cb, target.image, vk::ImageLayout::transferSrcOptimal,
			target.image, vk::ImageLayout::transferDstOptimal, {{blit}},
			vk::Filter::linear);

		// change layout of current mip level to transferSrc for next mip level
		// we even do it for the last level so that we have consistent src
		// scopes for all levels.
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

	if(dst) {
		vk::ImageMemoryBarrier barrier;
		barrier.image = target.image;
		barrier.subresourceRange.baseMipLevel = 0u;
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

vk::Format findDepthFormat(const vpp::Device& dev) {
	vk::ImageCreateInfo img; // dummy for property checking
	img.extent = {1, 1, 1};
	img.mipLevels = 1;
	img.arrayLayers = 1;
	img.imageType = vk::ImageType::e2d;
	img.sharingMode = vk::SharingMode::exclusive;
	img.tiling = vk::ImageTiling::optimal;
	img.samples = vk::SampleCountBits::e1;
	img.usage = vk::ImageUsageBits::depthStencilAttachment;
	img.initialLayout = vk::ImageLayout::undefined;

	auto fmts = {
		vk::Format::d32Sfloat,
		vk::Format::d32SfloatS8Uint,
		vk::Format::d24UnormS8Uint,
		vk::Format::d16Unorm,
		vk::Format::d16UnormS8Uint,
	};
	auto features = vk::FormatFeatureBits::depthStencilAttachment |
		vk::FormatFeatureBits::sampledImage;
	return vpp::findSupported(dev, fmts, img, features);
}

bool isDepthFormat(vk::Format format) {
	switch(format) {
		case vk::Format::d32Sfloat:
		case vk::Format::d32SfloatS8Uint:
		case vk::Format::d24UnormS8Uint:
		case vk::Format::d16Unorm:
		case vk::Format::d16UnormS8Uint:
			return true;
		default:
			return false;
	}
}

vk::SamplerCreateInfo linearSamplerInfo() {
	vk::SamplerCreateInfo sci;
	sci.addressModeU = vk::SamplerAddressMode::clampToEdge;
	sci.addressModeV = vk::SamplerAddressMode::clampToEdge;
	sci.addressModeW = vk::SamplerAddressMode::clampToEdge;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.mipmapMode = vk::SamplerMipmapMode::linear;
	sci.minLod = 0.0;
	sci.maxLod = 100.0;
	sci.anisotropyEnable = false;
	return sci;
}

} // namespace tkn
