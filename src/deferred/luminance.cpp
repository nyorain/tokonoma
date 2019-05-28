#include "luminance.hpp"
#include <stage/f16.hpp>

#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <dlg/dlg.hpp>
#include <cstdlib>

#include <shaders/deferred.luminance.comp.h>
#include <shaders/deferred.luminance.frag.h>
#include <shaders/deferred.luminanceMip.comp.h>

// TODO: even when compute shaders aren't available we can make the
// luminance calculation using mipmaps correct by simply using
// a power-of-two luminance target, clearing it with black before
// rendering (via renderpass, then just use a smaller viewport+scissor) and
// applying the final factor onto the result like we do with compute
// shaders.
// TODO(optimization): we could already sample (convert to log(luminance))
// and add multiple values per invocation in the extract stage and
// start with a lower resolution of target (e.g. 0.5 * size). Less
// memory needed and might be a bit faster.
// can't use linear sampler though since we need log before calculating
// average.

namespace {
vpp::ViewableImageCreateInfo targetInfo(vk::Extent2D size, bool compute) {
	vk::ImageUsageFlags usage;
	if(compute) {
		usage = vk::ImageUsageBits::storage |
			vk::ImageUsageBits::transferSrc |
			// vk::ImageUsageBits::transferDst | // TODO, for clearing
			vk::ImageUsageBits::sampled;
	} else {
		usage = vk::ImageUsageBits::colorAttachment |
			vk::ImageUsageBits::sampled | // only for debug presentation
			vk::ImageUsageBits::transferSrc |
			vk::ImageUsageBits::transferDst;
	}

	return {LuminancePass::format, vk::ImageAspectBits::color, size, usage};
}
} // anon namespace

void LuminancePass::create(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;
	auto& dev = wb.dev;
	extract_.rp = {}; // reset

	// test if r16f can be used as storage image. Supported on pretty
	// much all desktop systems.
	// we use a dummy size here but that shouldn't matter
	compute &= vpp::supported(dev, targetInfo({1024, 1024}, true).img);
	if(!compute) {
		dlg_info("Can't use compute pipeline for Luminance");
		auto stage = vk::ShaderStageBits::fragment;

		// render pass
		vk::AttachmentDescription attachment;
		attachment.format = format;
		attachment.samples = vk::SampleCountBits::e1;
		attachment.loadOp = vk::AttachmentLoadOp::dontCare;
		attachment.storeOp = vk::AttachmentStoreOp::store;
		attachment.stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachment.stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachment.initialLayout = vk::ImageLayout::undefined;
		attachment.finalLayout = vk::ImageLayout::transferSrcOptimal;

		vk::AttachmentReference colorRef;
		colorRef.attachment = 0u;
		colorRef.layout = vk::ImageLayout::colorAttachmentOptimal;

		vk::SubpassDescription subpass;
		subpass.colorAttachmentCount = 1u;
		subpass.pColorAttachments = &colorRef;

		vk::RenderPassCreateInfo rpi;
		rpi.subpassCount = 1u;
		rpi.pSubpasses = &subpass;
		rpi.attachmentCount = 1u;
		rpi.pAttachments = &attachment;

		extract_.rp = {dev, rpi};
		vpp::nameHandle(extract_.rp, "LuminancePass:extract_.rp");

		// pipeline
		auto extractBindings = {
			// nearest sampler since we don't interpolate
			vpp::descriptorBinding( // input light tex
				vk::DescriptorType::combinedImageSampler,
				stage, -1, 1, &info.samplers.nearest),
		};

		extract_.dsLayout = {dev, extractBindings};
		vk::PushConstantRange pcr;
		pcr.size = sizeof(nytl::Vec3f);
		pcr.stageFlags = stage;
		extract_.pipeLayout = {dev, {{extract_.dsLayout.vkHandle()}}, {{pcr}}};

		vpp::ShaderModule lumShader(dev, deferred_luminance_frag_data);
		vpp::GraphicsPipelineInfo gpi {extract_.rp, extract_.pipeLayout, {{{
			{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
			{lumShader, vk::ShaderStageBits::fragment},
		}}}};

		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.attachmentCount = 1u;
		gpi.blend.pAttachments = &doi::noBlendAttachment();

		extract_.pipe = {dev, gpi.info()};
	} else {
		auto stage = vk::ShaderStageBits::compute;
		auto extractBindings = {
			vpp::descriptorBinding(vk::DescriptorType::storageImage, stage),
			vpp::descriptorBinding(vk::DescriptorType::storageImage, stage),
		};

		extract_.dsLayout = {dev, extractBindings};
		vk::PushConstantRange pcr;
		pcr.size = sizeof(nytl::Vec3f);
		pcr.stageFlags = stage;
		extract_.pipeLayout = {dev, {{extract_.dsLayout.vkHandle()}}, {{pcr}}};

		vpp::ShaderModule lumShader(dev, deferred_luminance_comp_data);
		ComputeGroupSizeSpec groupSizeSpec(extractGroupDimSize, extractGroupDimSize);

		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = extract_.pipeLayout;
		cpi.stage.module = lumShader;
		cpi.stage.stage = vk::ShaderStageBits::compute;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
		extract_.pipe = {dev, cpi};

		// sampler
		vk::SamplerCreateInfo sci;
		sci.addressModeU = vk::SamplerAddressMode::clampToBorder;
		sci.addressModeV = vk::SamplerAddressMode::clampToBorder;
		sci.minFilter = vk::Filter::linear;
		sci.magFilter = vk::Filter::linear;
		sci.borderColor = vk::BorderColor::floatOpaqueBlack;
		sci.anisotropyEnable = false;
		mip_.sampler = {dev, sci};
		vpp::nameHandle(mip_.sampler, "LuminancePass:mip_.sampler");

		// mip pipe
		auto mipBindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				stage, -1, 1, &mip_.sampler.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::storageImage, stage),
		};
		mip_.dsLayout = {dev, mipBindings};
		pcr.size = sizeof(nytl::Vec2u32);
		mip_.pipeLayout = {dev, {{mip_.dsLayout.vkHandle()}}, {{pcr}}};

		vpp::nameHandle(mip_.dsLayout, "LuminancePass:mip_.dsLayout");
		vpp::nameHandle(mip_.pipeLayout, "LuminancePass:mip_.pipeLayout");

		dlg_assertm((mipGroupDimSize & (mipGroupDimSize - 1)) == 0,
			"mipGroupDimSize must be a power of 2");
		vpp::ShaderModule mipShader(dev, deferred_luminanceMip_comp_data);
		groupSizeSpec = {mipGroupDimSize, mipGroupDimSize};

		cpi.layout = mip_.pipeLayout;
		cpi.stage.module = mipShader;
		cpi.stage.stage = vk::ShaderStageBits::compute;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
		mip_.pipe = {dev, cpi};
		vpp::nameHandle(mip_.pipe, "LuminancePass:mip_.pipe");

		// initial max level guess, enough for most
		static constexpr auto levels = 13;
		mip_.levels.resize(levels);
		data.initLevels.resize(levels);
		for(auto i = 0u; i < levels; ++i) {
			mip_.levels[i].ds = {data.initLevels[i], wb.alloc.ds,
				mip_.dsLayout};
		}
	}

	vpp::nameHandle(extract_.dsLayout, "LuminancePass:extract_.dsLayout");
	vpp::nameHandle(extract_.pipeLayout, "LuminancePass:extract_.pipeLayout");
	vpp::nameHandle(extract_.pipe, "LuminancePass:extract_.pipe");

	extract_.ds = {data.initDs, wb.alloc.ds, extract_.dsLayout};
	dstBuffer_ = {data.initDstBuffer, wb.alloc.bufHost, sizeof(doi::f16),
		vk::BufferUsageBits::transferDst, dev.hostMemoryTypes(), 2u};
}

void LuminancePass::init(InitData& data, const PassCreateInfo&) {
	dstBuffer_.init(data.initDstBuffer);
	dstBufferMap_ = dstBuffer_.memoryMap();

	extract_.ds.init(data.initDs);
	vpp::nameHandle(extract_.ds, "LuminancePass:extract_.ds");

	if(usingCompute()) {
		dlg_assert(data.initLevels.size() == mip_.levels.size());
		for(auto i = 0u; i < data.initLevels.size(); ++i) {
			mip_.levels[i].ds.init(data.initLevels[i]);
			auto name = "Luminance:mip_.levels[" + std::to_string(i) + "].ds";
			vpp::nameHandle(mip_.levels[i].ds, name.c_str());
		}
	}
}

void LuminancePass::createBuffers(InitBufferData& data,
		const doi::WorkBatcher& wb, vk::Extent2D size) {
	auto mem = wb.dev.deviceMemoryTypes();
	auto info = targetInfo(size, usingCompute()).img;
	auto levelCount = vpp::mipmapLevels(size); // full mip chain
	info.mipLevels = levelCount;
	dlg_assert(vpp::supported(wb.dev, info));
	target_ = {data.initTarget, wb.alloc.memDevice, info, mem};

	// yeah, we don't defer ds initialization here but it shouldn't matter
	// really since this only happens when the resolution is increased
	// beyond a point it never was before (which shouldn't happen
	// often).
	--levelCount;
	if(usingCompute() && levelCount >= mip_.levels.size()) {
		auto start = mip_.levels.size();
		mip_.levels.resize(levelCount);
		for(auto i = start; i < levelCount; ++i) {
			mip_.levels[i].ds = {wb.alloc.ds, mip_.dsLayout};
			auto name = "Luminance:mip_.levels[" + std::to_string(i) + "].ds";
			vpp::nameHandle(mip_.levels[i].ds, name.c_str());
		}
	}
}

void LuminancePass::initBuffers(InitBufferData& data, vk::ImageView light,
		vk::Extent2D size) {
	auto& dev = extract_.pipe.device();

	auto levelCount = vpp::mipmapLevels(size); // full mip chain
	auto ivi = targetInfo(size, usingCompute()).view;
	ivi.image = target_.image();
	target_.init(data.initTarget, ivi);
	vpp::nameHandle(target_.image(), "Luminance:target_.image");
	vpp::nameHandle(target_.imageView(), "Luminance:target_.imageView");

	if(usingCompute()) {
		dlg_assert(levelCount - 1 < mip_.levels.size());
		vpp::DescriptorSetUpdate dsu(extract_.ds);
		dsu.storage({{{}, light, vk::ImageLayout::general}});
		dsu.storage({{{}, target_.imageView(), vk::ImageLayout::general}});
		dsu.apply();

		// mip levels
		const auto mf = (mipGroupDimSize * 4); // minification factor
		const u32 shift = std::log2(mf); // mf is power always of two
		auto i = shift;

		auto prevView = target_.vkImageView();
		auto* prevTarget = &mip_.target0;
		auto isize = size;
		auto totalSum = 1;
		while(isize.width > 1 || isize.height > 1) {
			totalSum *= mf * mf;

			auto mipi = doi::mipmapSize(size, i);
			isize.width = std::ceil(isize.width / float(mf));
			isize.height = std::ceil(isize.height / float(mf));
			if(isize.width > mipi.width || isize.height > mipi.height) {
				dlg_assertm(i != levelCount - 1, "{} {} {} {}",
					isize.width, isize.height, mipi.width, mipi.height);
				--i;
			}
			*prevTarget = i;
			prevTarget = &mip_.levels[i].target;

			// size.width = std::max((size.width + mf - 1) >> shift, 1u); // ceil
			// size.height = std::max((size.height + mf - 1) >> shift, 1u); // ceil

			ivi.subresourceRange.baseMipLevel = i;
			mip_.levels[i].view = {dev, ivi};
			auto name = "Luminance:mip_.levels[" + std::to_string(i) + "].view";
			vpp::nameHandle(mip_.levels[i].view, name.c_str());

			vpp::DescriptorSetUpdate dsu(mip_.levels[i].ds);
			dsu.imageSampler({{{}, prevView, vk::ImageLayout::shaderReadOnlyOptimal}});
			dsu.storage({{{}, mip_.levels[i].view, vk::ImageLayout::general}});

			prevView = mip_.levels[i].view;
			i = std::min(i + shift, levelCount - 1);
		}

		mip_.factor = totalSum / float(size.width * size.height);
		dlg_info("factor: {}", mip_.factor);
	} else {
		vk::FramebufferCreateInfo fbi = {{}, extract_.rp,
			1, &target_.vkImageView(), size.width, size.height, 1};
		extract_.fb = {dev, fbi};

		vpp::DescriptorSetUpdate dsu(extract_.ds);
		dsu.imageSampler({{{}, light, vk::ImageLayout::shaderReadOnlyOptimal}});
		mip_.factor = 1.f;
	}
}

void LuminancePass::record(vk::CommandBuffer cb, RenderTarget& light,
		vk::Extent2D size) {
	vpp::DebugLabel(cb, "LuminancePass");
	auto levelCount = vpp::mipmapLevels(size); // full mip chain
	auto width = size.width;
	auto height = size.height;

	if(usingCompute()) {
		vk::cmdPushConstants(cb, extract_.pipeLayout,
			vk::ShaderStageBits::compute, 0, sizeof(luminance), &luminance);

		vk::ImageMemoryBarrier barrier;
		/*
		// NOTE: we clamp in compute shader instead, pass the real
		// current read size as push constant
		barrier.image = target_.image();
		barrier.subresourceRange.layerCount = 1;
		barrier.subresourceRange.levelCount = levelCount;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.srcAccessMask = {};
		barrier.newLayout = vk::ImageLayout::transferDstOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferWrite;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});

		vk::cmdClearColorImage(cb, target_.image(),
			vk::ImageLayout::transferDstOptimal, {{0.f, 0.f, 0.f, 0.f}},
			{{{vk::ImageAspectBits::color, 0, levelCount, 0, 1}}});
		*/

		// transition all levels to general layout, discard previous content
		barrier.image = target_.image();
		barrier.subresourceRange.layerCount = 1;
		barrier.subresourceRange.levelCount = levelCount;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		// barrier.oldLayout = vk::ImageLayout::transferDstOptimal;
		// barrier.srcAccessMask = vk::AccessBits::transferWrite;
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.srcAccessMask = {};
		barrier.newLayout = vk::ImageLayout::general;
		barrier.dstAccessMask = vk::AccessBits::shaderWrite;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe | vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier}});

		transitionRead(cb, light, vk::ImageLayout::general,
			vk::PipelineStageBits::computeShader, vk::AccessBits::shaderRead);
		u32 cx = std::ceil(width / float(extractGroupDimSize));
		u32 cy = std::ceil(height / float(extractGroupDimSize));
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, extract_.pipe);
		doi::cmdBindComputeDescriptors(cb, extract_.pipeLayout, 0, {extract_.ds});
		vk::cmdDispatch(cb, cx, cy, 1);

		const auto mf = (mipGroupDimSize * 4); // minification factor
		auto prevLevel = 0u;
		auto i = mip_.target0;
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, mip_.pipe);
		while(width > 1 || height > 1) {
			dlg_assert(i > 0 && i < mip_.levels.size());
			auto& level = mip_.levels[i];

			// make sure writing has finished; transition
			vk::ImageMemoryBarrier barrier;
			barrier.image = target_.image();
			barrier.subresourceRange.layerCount = 1;
			barrier.subresourceRange.levelCount = 1;
			barrier.subresourceRange.baseMipLevel = prevLevel;
			barrier.subresourceRange.aspectMask = vk::ImageAspectBits::color;
			barrier.oldLayout = vk::ImageLayout::general;
			barrier.srcAccessMask = vk::AccessBits::shaderWrite;
			barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
			barrier.dstAccessMask = vk::AccessBits::shaderRead;
			vk::cmdPipelineBarrier(cb,
				vk::PipelineStageBits::computeShader,
				vk::PipelineStageBits::computeShader,
				{}, {}, {}, {{barrier}});

			auto size = nytl::Vec2u32{width, height};
			vk::cmdPushConstants(cb, mip_.pipeLayout, vk::ShaderStageBits::compute,
				0, sizeof(size), &size);
			width = std::ceil(width / float(mf));
			height = std::ceil(height / float(mf));
			doi::cmdBindComputeDescriptors(cb, mip_.pipeLayout, 0, {level.ds});
			vk::cmdDispatch(cb, width, height, 1);
			prevLevel = i;
			i = level.target;
		}

		dlg_assert(prevLevel == levelCount - 1);

		// make sure writing to last mip has finished
		// make sure transfer to last level has completed
		vk::ImageMemoryBarrier barrierLast;
		barrierLast.image = target_.image();
		barrierLast.subresourceRange.layerCount = 1;
		barrierLast.subresourceRange.levelCount = 1;
		barrierLast.subresourceRange.baseMipLevel = levelCount - 1;
		barrierLast.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		barrierLast.oldLayout = vk::ImageLayout::general;
		barrierLast.srcAccessMask = vk::AccessBits::shaderWrite;
		barrierLast.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrierLast.dstAccessMask = vk::AccessBits::transferRead;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrierLast}});
	} else {
		vk::cmdPushConstants(cb, extract_.pipeLayout,
			vk::ShaderStageBits::fragment, 0, sizeof(luminance), &luminance);

		transitionRead(cb, light, vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::fragmentShader, vk::AccessBits::shaderRead);
		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdBeginRenderPass(cb, {extract_.rp, extract_.fb,
			{0u, 0u, width, height}, 0, nullptr}, {});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, extract_.pipe);
		doi::cmdBindGraphicsDescriptors(cb, extract_.pipeLayout, 0, {extract_.ds});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);

		doi::DownscaleTarget target;
		target.image = target_.image();
		target.srcAccess = vk::AccessBits::colorAttachmentWrite;
		target.srcStages = vk::PipelineStageBits::colorAttachmentOutput;
		target.layout = vk::ImageLayout::transferSrcOptimal;
		target.layerCount = 1;
		target.width = width;
		target.height = height;
		doi::downscale(cb, target, levelCount - 1);

		// make sure transfer to last level has completed
		vk::ImageMemoryBarrier barrierLast;
		barrierLast.image = target_.image();
		barrierLast.subresourceRange.layerCount = 1;
		barrierLast.subresourceRange.levelCount = 1;
		barrierLast.subresourceRange.baseMipLevel = levelCount - 1;
		barrierLast.subresourceRange.aspectMask = vk::ImageAspectBits::color;
		barrierLast.oldLayout = vk::ImageLayout::transferSrcOptimal;
		barrierLast.srcAccessMask = vk::AccessBits::transferRead |
			vk::AccessBits::transferWrite;
		barrierLast.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrierLast.dstAccessMask = vk::AccessBits::transferRead;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrierLast}});
	}

	vk::BufferImageCopy copy;
	copy.bufferOffset = dstBuffer_.offset();
	copy.imageExtent = {1, 1, 1};
	copy.imageOffset = {0, 0, 0};
	copy.imageSubresource.aspectMask = vk::ImageAspectBits::color;
	copy.imageSubresource.layerCount = 1;
	copy.imageSubresource.mipLevel = levelCount - 1;
	vk::cmdCopyImageToBuffer(cb, target_.image(),
		vk::ImageLayout::transferSrcOptimal, dstBuffer_.buffer(), {{copy}});

	// TODO: probably not needed, right? queueSubmit and the fence wait
	// inserts such a dependency automatically iirc
	vk::BufferMemoryBarrier bb;
	bb.buffer = dstBuffer_.buffer();
	bb.offset = dstBuffer_.offset();
	bb.size = dstBuffer_.size();
	bb.srcAccessMask = vk::AccessBits::transferWrite;
	bb.dstAccessMask = vk::AccessBits::hostRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
		vk::PipelineStageBits::host, {}, {}, {{bb}}, {});
}

float LuminancePass::updateDevice() {
	dstBufferMap_.invalidate();
	auto span = dstBufferMap_.span();
	auto v = float(doi::read<doi::f16>(span));
	return std::exp2(mip_.factor * v);
}

SyncScope LuminancePass::dstScopeLight() const {
	SyncScope scope;
	scope.layout = vk::ImageLayout::shaderReadOnlyOptimal;
	scope.access = vk::AccessBits::shaderRead;
	scope.stages = usingCompute() ?
		vk::PipelineStageBits::computeShader :
		vk::PipelineStageBits::fragmentShader;
	return scope;
}

SyncScope LuminancePass::srcScopeTarget() const {
	SyncScope scope;
	if(usingCompute()) {
		scope.layout = vk::ImageLayout::shaderReadOnlyOptimal;
		scope.access = vk::AccessBits::shaderRead;
		scope.stages = vk::PipelineStageBits::computeShader;
	} else {
		scope.layout = vk::ImageLayout::transferSrcOptimal;
		scope.access = vk::AccessBits::transferRead;
		scope.stages = vk::PipelineStageBits::transfer;
	}
	return scope;
}
