#include <tkn/passes/bloom.hpp>
#include <vpp/debug.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>

// TODO: we could create the temporary target with just one level and then
// use that for all blurs. GaussianBlur does not allow this atm, the
// shader assumes src and dst have same size, i.e. would get the offsets wrong.

namespace tkn {

void BloomPass::createBuffers(InitBufferData& data, tkn::WorkBatcher& wb,
		unsigned levels, vk::Extent2D size, const GaussianBlur& blur) {
	auto& dev = wb.dev;
	auto usage = vk::ImageUsageBits::storage |
		vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, size, usage);
	dlg_assert(vpp::supported(dev, info.img));
	if(levels > 10u) {
		dlg_warn("maxLevels is unrealisticly high: {}", levels);
		levels = 10u;
	}

	size.width = std::max(size.width >> 1, 1u);
	size.height = std::max(size.height >> 1, 1u);
	dlg_assert(levels <= vpp::mipmapLevels(size));

	info.img.extent.depth = 1;
	info.img.mipLevels = levelCount_;
	tmpTarget_ = {data.initTmpTarget, wb.alloc.memDevice, info.img,
		dev.deviceMemoryTypes()};
	data.viewInfo = info.view;

	if(levelCount_ >= levels_.size()) {
		auto start = levels_.size();
		auto newCount = levelCount_ - start;
		levels_.resize(levelCount_);
		data.initBlurs.resize(newCount);
		for(auto i = 0u; i < newCount; ++i) {
			levels_[start + i].blur = blur.createInstance(data.initBlurs[i]);
		}
	}
}

void BloomPass::initBuffers(InitBufferData& data, vk::Image highlightImage,
		vk::ImageView highlightView, const GaussianBlur& blur) {
	auto& dev =	blur.device();
	tmpTarget_.init(data.initTmpTarget);
	vpp::nameHandle(tmpTarget_, "BloomPass:tmpTarget");

	auto newLevels = data.initBlurs.size();
	for(auto i = 0u; i < newLevels; ++i) {
		auto& ini = levels_[levels_.size() - newLevels + i].blur;
		blur.initInstance(ini, data.initBlurs[i]);
	}

	auto ivi = data.viewInfo;
	for(auto i = 0u; i < levelCount_; ++i) {
		ivi.subresourceRange.baseMipLevel = i;
		ivi.image = highlightImage;
		vk::ImageView targetView = highlightView;
		if(i != 0) {
			levels_[i].target = {dev, ivi};
			targetView = levels_[i].target;
		}

		ivi.image = tmpTarget_;
		levels_[i].tmp = {dev, ivi};
		blur.updateInstance(levels_[i].blur, targetView, levels_[i].tmp);
	}

	ivi.image = highlightImage;
	ivi.subresourceRange.baseMipLevel = 0;
	ivi.subresourceRange.levelCount = levelCount_;
	fullView_ = {dev, ivi};
	vpp::nameHandle(fullView_, "BloomPass:fullView_");
}

void BloomPass::record(vk::CommandBuffer cb, vk::Image highlight,
		vk::Extent2D size, const GaussianBlur& blur) {

	// transition all mipmaps but first to transferDstOptimal
	// mip 0 will be blitted to from emission buffer, the other ones
	// from the previous mip level
	vk::ImageMemoryBarrier barrier;
	barrier.image = highlight;
	barrier.subresourceRange.layerCount = 1;
	barrier.subresourceRange.baseMipLevel = 1u;
	barrier.subresourceRange.levelCount = levelCount_ - 1;
	barrier.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	barrier.oldLayout = vk::ImageLayout::undefined; // discard levels
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::transferDstOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferWrite;

	vk::ImageMemoryBarrier bT;
	bT.image = tmpTarget_;
	bT.subresourceRange = {vk::ImageAspectBits::color, 0u, levelCount_, 0u, 1u};
	bT.oldLayout = vk::ImageLayout::undefined; // discard
	bT.newLayout = blur.dstScopeTmp().layout;
	bT.dstAccessMask = blur.dstScopeTmp().access;

	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::transfer | blur.dstScopeTmp().stages,
		{}, {}, {}, {{barrier, bT}});

	auto kernel = blur.createKernel(blurHSize, blurFac);
	auto srcScope = tkn::SyncScope {};
	for(auto i = 0u; i < levelCount_; ++i) {
		auto w = std::max(size.width >> (i + 1), 1u);
		auto h = std::max(size.height >> (i + 1), 1u);

		if(mipBlurred) {
			if(srcScope.access) {
				barrier.subresourceRange.baseMipLevel = i;
				barrier.subresourceRange.levelCount = 1;
				barrier.srcAccessMask = srcScope.access;;
				barrier.oldLayout = srcScope.layout;
				barrier.newLayout = blur.dstScope().layout;
				barrier.dstAccessMask = blur.dstScope().access;
				vk::cmdPipelineBarrier(cb, srcScope.stages,
					vk::PipelineStageBits::computeShader,
					{}, {}, {}, {{barrier}});
			}

			GaussianBlur::Image srcDst = {highlight, i, 0u};
			GaussianBlur::Image tmp = {tmpTarget_, i, 0u};
			blur.record(cb, levels_[i].blur, {w, h}, srcDst, tmp, kernel);

			srcScope.access = blur.srcScope().access;
			srcScope.stages = blur.srcScope().stages;
			srcScope.layout = blur.srcScope().layout;
		}

		if(srcScope.access) {
			barrier.subresourceRange.baseMipLevel = i;
			barrier.subresourceRange.levelCount = 1;
			barrier.srcAccessMask = srcScope.access;
			barrier.oldLayout = srcScope.layout;
			barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
			barrier.dstAccessMask = vk::AccessBits::transferRead;
			vk::cmdPipelineBarrier(cb, srcScope.stages,
				vk::PipelineStageBits::transfer,
				{}, {}, {}, {{barrier}});
		}

		// Copy to next level, if there is one.
		// We do this check *after* transitioning the current level
		// so that all levels are consistenly in transferSrcOptimal
		// layout, even the last one where we never needed it.
		// Makes handling barriers down the line easier.
		if(i + 1 > levelCount_) {
			break;
		}

		vk::ImageBlit blit;
		blit.srcSubresource.layerCount = 1;
		blit.srcSubresource.mipLevel = i;

		blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.srcOffsets[1].x = w;
		blit.srcOffsets[1].y = h;
		blit.srcOffsets[1].z = 1u;

		w = std::max(size.width >> (i + 2), 1u);
		h = std::max(size.height >> (i + 2), 1u);
		blit.dstSubresource.layerCount = 1;
		blit.dstSubresource.mipLevel = i + 1;
		blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.dstOffsets[1].x = w;
		blit.dstOffsets[1].y = h;
		blit.dstOffsets[1].z = 1u;

		vk::cmdBlitImage(cb, highlight, vk::ImageLayout::transferSrcOptimal,
			highlight, vk::ImageLayout::transferDstOptimal, {{blit}},
			vk::Filter::linear);

		// for the next level
		srcScope.stages = vk::PipelineStageBits::transfer;
		srcScope.access = vk::AccessBits::transferWrite;
		srcScope.layout = vk::ImageLayout::transferDstOptimal;
	}

	if(!mipBlurred) {
		// transition all levels for blur pass
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = levelCount_;
		barrier.oldLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.srcAccessMask = vk::AccessBits::transferRead;
		barrier.newLayout = blur.dstScope().layout;
		barrier.dstAccessMask = blur.dstScope().access;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
			blur.dstScope().stages,
			{}, {}, {}, {{barrier}});

		for(auto i = 0u; i < levelCount_; ++i) {
			auto w = std::max(size.width >> (i + 1), 1u);
			auto h = std::max(size.height >> (i + 1), 1u);
			GaussianBlur::Image srcDst = {highlight, i, 0u};
			GaussianBlur::Image tmp = {tmpTarget_, i, 0u};
			blur.record(cb, levels_[i].blur, {w, h}, srcDst, tmp, kernel);
		}
	}
}

SyncScope BloomPass::dstScopeHighlight() const {
	return mipBlurred ? GaussianBlur::dstScope() : SyncScope {
		vk::PipelineStageBits::transfer,
		vk::ImageLayout::transferSrcOptimal,
		vk::AccessBits::transferRead,
	};
}

SyncScope BloomPass::srcScopeHighlight() const {
	return mipBlurred ? SyncScope {
		vk::PipelineStageBits::transfer,
		vk::ImageLayout::transferSrcOptimal,
		// may seem weird to use transferRead as src scope for a
		// newly written target but a pipeline
		// barrier again these access/stages will transitively be against
		// all writes
		vk::AccessBits::transferRead,
	} : GaussianBlur::srcScope();
}

} // namespace tkn
