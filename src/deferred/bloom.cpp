#include "bloom.hpp"
#include <tkn/bits.hpp>
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/formats.hpp>

#include <shaders/deferred.bloom.comp.h>
#include <shaders/deferred.gblur.comp.h>

void BloomPass::create(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;
	auto& dev = wb.dev;

	// work group size spec info
	ComputeGroupSizeSpec groupSizeSpec(groupDimSize, groupDimSize);

	// filter
	auto filterBindings = {
		// important to use linear sampler here, since we implicitly downscale
		// the light buffer
		vpp::descriptorBinding( // input light buffer
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &info.samplers.linear),
		vpp::descriptorBinding( // output color
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute)
	};

	filter_.dsLayout = {dev, filterBindings};
	filter_.pipeLayout = {dev, {{filter_.dsLayout.vkHandle()}}, {}};
	vpp::nameHandle(filter_.dsLayout, "BloomPass:filter_.dsLayout");
	vpp::nameHandle(filter_.pipeLayout, "BloomPass:filter_.pipeLayout");

	// pipe
	vpp::ShaderModule filterShader(dev, deferred_bloom_comp_data);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = filter_.pipeLayout;
	cpi.stage.module = filterShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
	filter_.pipe = {dev, cpi};
	vpp::nameHandle(filter_.pipe, "BloomPass:filter_.pipe");

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
	vpp::nameHandle(blur_.dsLayout, "BloomPass:blur_.dsLayout");
	vpp::nameHandle(blur_.pipeLayout, "BloomPass:blur_.pipeLayout");

	// pipe
	vpp::ShaderModule blurShader(dev, deferred_gblur_comp_data);

	cpi.layout = blur_.pipeLayout;
	cpi.stage.module = blurShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
	blur_.pipe = {dev, cpi};
	vpp::nameHandle(blur_.pipeLayout, "BloomPass:blur_.pipe");

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

	filter_.ubo = {data.initUbo, wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
}

void BloomPass::init(InitData& data, const PassCreateInfo&) {
	filter_.ubo.init(data.initUbo);
	filter_.uboMap = filter_.ubo.memoryMap();

	filter_.ds.init(data.initFilterDs);
	vpp::nameHandle(blur_.pipeLayout, "BloomPass:filter_.ds");

	for(auto i = 0u; i < maxLevels; ++i) {
		tmpLevels_[i].ds.init(data.initTmpLevels[i]);
		targetLevels_[i].ds.init(data.initTargetLevels[i]);
		vpp::nameHandle(tmpLevels_[i].ds,
			dlg::format("BloomPass:tmpLevels_[{}].ds", i).c_str());
		vpp::nameHandle(targetLevels_[i].ds,
			dlg::format("BloomPass:targetLevels_[{}].ds", i).c_str());
	}
}

void BloomPass::createBuffers(InitBufferData& data, const tkn::WorkBatcher& wb,
		const vk::Extent2D& size) {
	auto& dev = wb.dev;
	auto usage = vk::ImageUsageBits::storage |
		vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, size, usage);
	dlg_assert(vpp::supported(dev, info.img));
	if(maxLevels > 12) {
		dlg_warn("maxLevels is unrealisticly high: {}", maxLevels);
		maxLevels = 3u;
	}

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

	// yeah, we don't defer ds initialization here but it shouldn't matter
	// really since this only happens when maxLevels is increased
	if(levelCount_ >= tmpLevels_.size()) {
		dlg_assert(tmpLevels_.size() == targetLevels_.size());
		auto start = tmpLevels_.size();
		tmpLevels_.resize(levelCount_);
		targetLevels_.resize(levelCount_);
		for(auto i = start; i < levelCount_; ++i) {
			tmpLevels_[i].ds = {wb.alloc.ds, blur_.dsLayout};
			targetLevels_[i].ds = {wb.alloc.ds, blur_.dsLayout};
		}
	}
}

void BloomPass::initBuffers(InitBufferData& data, vk::ImageView lightInput) {
	auto& dev = blur_.pipe.device();
	target_.init(data.initTarget);
	tmpTarget_.init(data.initTmpTarget);
	vpp::nameHandle(target_, "BloomPass:target_");
	vpp::nameHandle(tmpTarget_, "BloomPass:tmpTarget_");

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
		dsu.storage({{{}, tmpLevels_[i].view,
			vk::ImageLayout::general}});
		dsu.apply();

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
	vpp::nameHandle(fullView_, "BloomPass:fullView_");

	vpp::DescriptorSetUpdate dsu(filter_.ds);
	dsu.imageSampler({{{}, lightInput,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.storage({{{}, targetLevels_[0].view,
		vk::ImageLayout::general}});
	dsu.uniform({{{filter_.ubo}}});
}

void BloomPass::recordBlur(vk::CommandBuffer cb, unsigned mip, vk::Extent2D size) {
	auto lname = dlg::format("recordBlur:{}", mip);
	vpp::DebugLabel debugLabel(target_.device(), cb, lname.c_str());

	auto& tmpl = tmpLevels_[mip];
	auto& targetl = targetLevels_[mip];

	u32 horizontal = 1u;
	vk::cmdPushConstants(cb, blur_.pipeLayout, vk::ShaderStageBits::compute,
		0, 4, &horizontal);
	tkn::cmdBindComputeDescriptors(cb, blur_.pipeLayout, 0, {tmpl.ds});

	auto w = std::max(size.width >> (mip + 1), 1u);
	auto h = std::max(size.height >> (mip + 1), 1u);
	unsigned cx = std::ceil(w / float(groupDimSize));
	unsigned cy = std::ceil(h / float(groupDimSize));
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
	barrier.oldLayout = vk::ImageLayout::undefined; // overwrite
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange.baseMipLevel = mip;
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::computeShader,
		{}, {}, {}, {{barrier}});

	horizontal = 0u;
	vk::cmdPushConstants(cb, blur_.pipeLayout, vk::ShaderStageBits::compute,
		0, 4, &horizontal);
	tkn::cmdBindComputeDescriptors(cb, blur_.pipeLayout, 0, {targetl.ds});
	vk::cmdDispatch(cb, cx, cy, 1);
}

void BloomPass::record(vk::CommandBuffer cb, vk::Image emission,
		vk::Extent2D size) {
	vpp::DebugLabel debugLabel(target_.device(), cb, "BloomPass");

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
	// make tmp target writable for blur passes
	vk::ImageMemoryBarrier barrier;
	barrier.image = tmpTarget_;
	barrier.subresourceRange.layerCount = 1;
	barrier.subresourceRange.levelCount = levelCount_;
	barrier.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	barrier.oldLayout = vk::ImageLayout::undefined; // overwritten
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
			src = emission;
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

		vk::cmdBlitImage(cb, src, vk::ImageLayout::transferSrcOptimal,
			target_, vk::ImageLayout::transferDstOptimal, {{blit}},
			vk::Filter::linear);

		vk::ImageLayout layout = vk::ImageLayout::transferDstOptimal;
		vk::AccessFlags srcAccess = vk::AccessBits::transferWrite;
		vk::PipelineStageFlags srcStage = vk::PipelineStageBits::transfer;
		if(i == 0) {
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

			tkn::cmdBindComputeDescriptors(cb, filter_.pipeLayout, 0, {filter_.ds});
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, filter_.pipe);
			vk::cmdDispatch(cb,
				std::ceil(w / float(groupDimSize)),
				std::ceil(h / float(groupDimSize)), 1);

			layout = barrier.newLayout;
			srcAccess = barrier.dstAccessMask;
			srcStage = vk::PipelineStageBits::computeShader;
		}

		if(mipBlurred) {
			// blurring initially reads from target_, so make
			// it shaderReadOnlyOptimal
			barrier.oldLayout = layout;
			barrier.srcAccessMask = srcAccess;
			barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
			barrier.dstAccessMask = vk::AccessBits::shaderRead;
			vk::cmdPipelineBarrier(cb, srcStage,
				vk::PipelineStageBits::computeShader,
				{}, {}, {}, {{barrier}});

			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, blur_.pipe);
			recordBlur(cb, i, size);

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
	}
}

void BloomPass::updateDevice() {
	auto span = filter_.uboMap.span();
	tkn::write(span, params);
	filter_.uboMap.flush();
}

SyncScope BloomPass::dstScopeEmission() const {
	return {
		vk::PipelineStageBits::transfer,
		vk::ImageLayout::transferSrcOptimal,
		vk::AccessBits::transferRead,
	};
}

SyncScope BloomPass::dstScopeLight() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead
	};
}

SyncScope BloomPass::srcScopeTarget() const {
	if(mipBlurred) {
		return {
			vk::PipelineStageBits::transfer,
			vk::ImageLayout::transferSrcOptimal,
			// may seem weird to use transferRead as src scope for a
			// newly written target but a pipeline
			// barrier again these access/stages will transitively be against
			// all writes
			vk::AccessBits::transferRead,
		};
	} else {
		return {
			vk::PipelineStageBits::computeShader,
			vk::ImageLayout::general,
			vk::AccessBits::shaderWrite,
		};
	}
}
