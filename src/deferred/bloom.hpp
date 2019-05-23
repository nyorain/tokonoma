#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.bloom.comp.h>
#include <shaders/deferred.gblur.comp.h>

/// Bright-color-pass filter for bloom. Blurs the bloom/emission buffer
/// on multiple mip map levels.
class BloomPass {
public:
	static constexpr unsigned maxLevels = 4u;
	static constexpr unsigned groupSize = 32u;
	static constexpr bool mipBlurred = true;

	// static constexpr vk::Format format = GeometryPass::emissionFormat;
	static constexpr vk::Format format = vk::Format::r16g16b16a16Sfloat;

	struct InitData {
		std::vector<vpp::TrDs::InitData> initTmpLevels;
		std::vector<vpp::TrDs::InitData> initTargetLevels;
		vpp::TrDs::InitData initFilterDs;
	};

	struct InitBufferData {
		vpp::Image::InitData initTmpTarget;
		vpp::Image::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	BloomPass() = default;
	BloomPass(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);

	void createBuffers(InitBufferData&, const vk::Extent2D&,
		const doi::WorkBatcher&);
	void initBuffers(InitBufferData&, vk::ImageView lightInput);

	// Returns the bloom target in transferSrc/general layout,
	// depending on the 'mipBlurred' setting.
	// ImageView covers all bloom mip levels.
	RenderTarget record(vk::CommandBuffer cb, RenderTarget& emission,
		RenderTarget& light, vk::Extent2D);

	unsigned levelCount() const { return levelCount_; }

protected:
	void recordBlur(vk::CommandBuffer, unsigned mip, vk::Extent2D);

	struct BloomLevel {
		vpp::ImageView view;
		vpp::TrDs ds;
	};

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
	} filter_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} blur_;

	vpp::Image target_;
	std::vector<BloomLevel> targetLevels_;

	vpp::Image tmpTarget_; // for first step of blur
	std::vector<BloomLevel> tmpLevels_;

	vpp::ImageView fullView_;
	unsigned levelCount_;
};

// impl
BloomPass::BloomPass(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;
	auto& dev = wb.dev;

	// filter
	auto filterBindings = {
		// important to use linear sampler here, since we implicitly downscale
		// the light buffer
		vpp::descriptorBinding( // input light buffer
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &info.samplers.linear),
		vpp::descriptorBinding( // output color, blurred
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute)
	};

	filter_.dsLayout = {dev, filterBindings};
	filter_.pipeLayout = {dev, {{filter_.dsLayout.vkHandle()}}, {}};

	// pipe
	vpp::ShaderModule filterShader(dev, deferred_bloom_comp_data);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = blur_.pipeLayout;
	cpi.stage.module = filterShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	filter_.pipe = {dev, cpi};

	// blur
	auto blurBindings = {
		// important to use linear sampler here, our shader is optimized,
		// reads multiple pixels in a single fetch via linear sampling
		vpp::descriptorBinding( // input color
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &info.samplers.linear),
		vpp::descriptorBinding( // output color, blurred
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute)
	};

	vk::PushConstantRange pcr;
	pcr.size = 4;
	pcr.stageFlags = vk::ShaderStageBits::compute;

	blur_.dsLayout = {dev, blurBindings};
	blur_.pipeLayout = {dev, {{blur_.dsLayout.vkHandle()}}, {{pcr}}};

	// pipe
	vpp::ShaderModule blurShader(dev, deferred_gblur_comp_data);

	cpi.layout = blur_.pipeLayout;
	cpi.stage.module = blurShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	blur_.pipe = {dev, cpi};

	// descriptors
	filter_.ds = {data.initFilterDs, wb.alloc.ds, filter_.dsLayout};

	data.initTargetLevels.resize(maxLevels);
	data.initTmpLevels.resize(maxLevels);

	tmpLevels_.resize(maxLevels);
	targetLevels_.resize(maxLevels);
	for(auto i = 0u; i < maxLevels; ++i) {
		tmpLevels_[i].ds = {data.initTmpLevels[i], wb.alloc.ds, blur_.dsLayout};
		targetLevels_[i].ds = {data.initTargetLevels[i], wb.alloc.ds, blur_.dsLayout};
	}
}

void BloomPass::init(InitData& data, const PassCreateInfo&) {
	filter_.ds.init(data.initFilterDs);
	for(auto i = 0u; i < maxLevels; ++i) {
		tmpLevels_[i].ds.init(data.initTmpLevels[i]);
		targetLevels_[i].ds.init(data.initTargetLevels[i]);
	}
}

void BloomPass::createBuffers(InitBufferData& data, const vk::Extent2D& size,
		const doi::WorkBatcher& wb) {
	auto& dev = blur_.pipe.device();
	auto usage = vk::ImageUsageBits::storage | vk::ImageUsageBits::sampled;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, size, usage);
	dlg_assert(vpp::supported(dev, info.img));

	levelCount_ = std::min(maxLevels, vpp::mipmapLevels(size));
	info.img.extent.width = std::max(size.width >> 1, 1u);
	info.img.extent.height = std::max(size.height >> 1, 1u);
	info.img.extent.depth = 1;
	info.img.mipLevels = levelCount_;
	target_ = {data.initTarget, wb.alloc.memDevice, info.img,
		dev.deviceMemoryTypes()};
	tmpTarget_ = {data.initTmpTarget, wb.alloc.memDevice, info.img,
		dev.deviceMemoryTypes()};
	data.viewInfo = info.view;
}

void BloomPass::initBuffers(InitBufferData& data, vk::ImageView lightInput) {
	auto& dev = blur_.pipe.device();

	auto ivi = data.viewInfo;
	for(auto i = 0u; i < levelCount_; ++i) {
		ivi.subresourceRange.baseMipLevel = i;
		ivi.image = tmpTarget_;
		tmpLevels_[i].view = {dev, ivi};

		ivi.image = target_;
		targetLevels_[i].view = {dev, ivi};

		vpp::DescriptorSetUpdate dsu(tmpLevels_[i].ds);
		dsu.imageSampler({{{}, targetLevels_[i].view,
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.storage({{{}, targetLevels_[i].view,
			vk::ImageLayout::general}});

		dsu = {targetLevels_[i].ds};
		dsu.imageSampler({{{}, tmpLevels_[i].view,
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.storage({{{}, targetLevels_[i].view,
			vk::ImageLayout::general}});
	}

	ivi.image = target_;
	ivi.subresourceRange.baseMipLevel = 0;
	ivi.subresourceRange.levelCount = levelCount_;
	fullView_ = {dev, ivi};

	vpp::DescriptorSetUpdate dsu(filter_.ds);
	dsu.imageSampler({{{}, lightInput,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.storage({{{}, targetLevels_[0].view,
		vk::ImageLayout::general}});
}

void BloomPass::recordBlur(vk::CommandBuffer cb, unsigned mip, vk::Extent2D size) {
	auto& tmpl = tmpLevels_[mip];
	auto& targetl = targetLevels_[mip];

	std::uint32_t horizontal = 1u;
	vk::cmdPushConstants(cb, blur_.pipeLayout, vk::ShaderStageBits::compute,
		0, 4, &horizontal);
	doi::cmdBindComputeDescriptors(cb, blur_.pipeLayout, {tmpl.ds});

	auto w = std::max(size.width >> (mip + 1), 1u);
	auto h = std::max(size.height >> (mip + 1), 1u);
	unsigned cx = std::ceil(w / float(groupSize));
	unsigned cy = std::ceil(h / float(groupSize));
	vk::cmdDispatch(cb, cx, cy, 1);

	vk::ImageMemoryBarrier barrier;
	barrier.image = tmpTarget_;
	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = vk::AccessBits::shaderRead |
		vk::AccessBits::shaderWrite,
	barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	barrier.dstAccessMask = vk::AccessBits::shaderRead;
	barrier.subresourceRange = {vk::ImageAspectBits::color, mip, 1, 0, 1};
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::allCommands,
		{}, {}, {}, {{barrier}});

	barrier.image = target_;
	barrier.oldLayout = vk::ImageLayout::undefined; // overwritten
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange.baseMipLevel = mip + 1;
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::computeShader,
		{}, {}, {}, {{barrier}});

	horizontal = 0u;
	vk::cmdPushConstants(cb, blur_.pipeLayout, vk::ShaderStageBits::compute,
		0, 4, &horizontal);
	doi::cmdBindComputeDescriptors(cb, blur_.pipeLayout, {targetl.ds});
	vk::cmdDispatch(cb, cx, cy, 1);
}

void BloomPass::record(vk::CommandBuffer cb, RenderTarget& emission,
		RenderTarget& light, vk::Extent2D size) {
	unsigned w = size.width;
	unsigned h = size.height;

	// The implementation here is rather complex, lots of barriers
	// from different mip levels. In the end it's a variation
	// of dynamically creating a mipmap chain, we just run addition
	// blur shaders in between to blur every mipmap level.
	// Blurring layer for layer (both passes every time)
	// might have cache advantages over first doing horizontal
	// for all and then doing vertical for all i guess

	// blit (downscale to 0.5 * size): emission to target mip 0
	// make sure we transfer from emission target
	transitionRead(cb, emission, vk::ImageLayout::transferSrcOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferRead);

	// make tmp target writable for blur passes
	vk::ImageMemoryBarrier barrier;
	barrier.image = tmpTarget_;
	barrier.subresourceRange.layerCount = 1;
	barrier.subresourceRange.levelCount = levelCount_;
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader,
		{}, {}, {}, {{barrier}});


	// transition all mipmaps to transferDstOptimal
	// mip 0 will be blitted to from emission buffer, the other ones
	// from the previous mip level
	barrier.image = target_;
	barrier.oldLayout = vk::ImageLayout::undefined; // will be overblitted
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::transferDstOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferWrite;
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::transfer,
		{}, {}, {}, {{barrier}});

	// create bloom mip levels
	// mip level 0 is a bit different, we blit the content from the emission
	// buffer there and then add the bright color values from the light buffer
	for(auto i = 0u; i < levelCount_; ++i) {
		barrier.subresourceRange.baseMipLevel = i;
		barrier.subresourceRange.levelCount = 1;
		vk::Image src = target_;

		vk::ImageBlit blit;
		blit.srcSubresource.layerCount = 1;
		if(i == 0) {
			src = emission.image;
			blit.srcSubresource.mipLevel = 0;
		} else {
			blit.srcSubresource.mipLevel = i - 1;
		}

		blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.srcOffsets[1].x = w;
		blit.srcOffsets[1].y = h;
		blit.srcOffsets[1].z = 1u;

		// std::max needed for end offsets when the texture is not
		// quadratic: then we would get 0 there although the mipmap
		// still has size 1
		w = std::max(size.width >> (i + 1), 1u);
		h = std::max(size.height >> (i + 1), 1u);
		blit.dstSubresource.layerCount = 1;
		blit.dstSubresource.mipLevel = i;
		blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.dstOffsets[1].x = w;
		blit.dstOffsets[1].y = h;
		blit.dstOffsets[1].z = 1u;

		vk::cmdBlitImage(cb, target_, vk::ImageLayout::transferSrcOptimal,
			target_, vk::ImageLayout::transferDstOptimal, {{blit}},
			vk::Filter::linear);

		vk::ImageLayout layout = vk::ImageLayout::transferDstOptimal;
		vk::AccessFlags srcAccess = vk::AccessBits::transferWrite;
		vk::PipelineStageFlags srcStage = vk::PipelineStageBits::transfer;
		if(i == 0) {
			// make sure light is in the required layout and all writes
			// have finished.
			transitionRead(cb, light, vk::ImageLayout::shaderReadOnlyOptimal,
				vk::PipelineStageBits::computeShader, vk::AccessBits::shaderRead);

			// change layout of mip level 0 to general to add the high filter
			// pass from the light buffer
			barrier.oldLayout = layout;
			barrier.srcAccessMask = srcAccess;
			barrier.newLayout = vk::ImageLayout::general;
			barrier.dstAccessMask = vk::AccessBits::shaderWrite |
				vk::AccessBits::shaderRead;
			vk::cmdPipelineBarrier(cb, srcStage,
				vk::PipelineStageBits::computeShader,
				{}, {}, {}, {{barrier}});

			doi::cmdBindComputeDescriptors(cb, filter_.pipeLayout, {filter_.ds});
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, filter_.pipe);
			vk::cmdDispatch(cb,
				std::ceil(w / float(groupSize)),
				std::ceil(h / float(groupSize)), 1);

			layout = barrier.newLayout;
			srcAccess = barrier.dstAccessMask;
			srcStage = vk::PipelineStageBits::computeShader;
		}

		if(mipBlurred) {
			// blurring initially reads from target_, so make
			// it shaderReadOnlyOptimal
			barrier.image = target_;
			barrier.oldLayout = layout;
			barrier.srcAccessMask = srcAccess;
			barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
			barrier.dstAccessMask = vk::AccessBits::shaderRead;
			vk::cmdPipelineBarrier(cb, srcStage,
				vk::PipelineStageBits::computeShader,
				{}, {}, {}, {{barrier}});

			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, blur_.pipe);
			recordBlur(cb, 0, size);

			// from recordBur
			layout = vk::ImageLayout::general;
			srcAccess = vk::AccessBits::shaderWrite;
			srcStage = vk::PipelineStageBits::computeShader;
		}

		// change layout of current mip level to transferSrc for next mip level
		barrier.oldLayout = layout;
		barrier.srcAccessMask = srcAccess;
		barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		vk::cmdPipelineBarrier(cb, srcStage, vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});
	}

	RenderTarget ret;
	ret.image = target_;
	ret.view = fullView_;
	ret.layout = vk::ImageLayout::transferSrcOptimal;
	// may seem weird to use transferRead as writeAccess but a pipeline
	// barrier again these access/stages will transitively be against
	// all writes
	ret.writeAccess = vk::AccessBits::transferRead;
	ret.writeStages = vk::PipelineStageBits::transfer;

	if(!mipBlurred) {
		// transform all levels back to readonly for blur read pass
		barrier.subresourceRange.baseMipLevel = 0u;
		barrier.subresourceRange.levelCount = levelCount_;
		barrier.oldLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.srcAccessMask = vk::AccessBits::transferRead;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier}});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, blur_.pipe);
		for(auto i = 0u; i < levelCount_; ++i) {
			recordBlur(cb, i, size);
		}

		// from recordBlur
		ret.layout = vk::ImageLayout::general;
		ret.writeAccess = vk::AccessBits::shaderWrite;
		ret.writeStages = vk::PipelineStageBits::computeShader;
	}

	// make all target layers readonly
	// barrier.image = target_;
	// barrier.oldLayout = layout;
	// barrier.srcAccessMask = srcAccess;
	// barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	// barrier.dstAccessMask = vk::AccessBits::shaderRead;
	// barrier.subresourceRange.baseMipLevel = 1u;
	// barrier.subresourceRange.levelCount = bloomLevels;
	// vk::cmdPipelineBarrier(cb,
	// 	vk::PipelineStageBits::allCommands,
	// 	vk::PipelineStageBits::allCommands,
	// 	{}, {}, {}, {{barrier}});
}
