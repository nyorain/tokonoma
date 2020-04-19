#include "taa.hpp"

#include <tkn/render.hpp>
#include <tkn/bits.hpp>
#include <vpp/vk.hpp>
#include <vpp/shader.hpp>
#include <vpp/debug.hpp>
#include <vpp/formats.hpp>
#include <dlg/dlg.hpp>

#include <shaders/taa.taa2.comp.h>

void TAAPass::init(vpp::Device& dev, vk::Sampler linearSampler) {
	auto taaBindings = {
		// linear sampler for history access important
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &linearSampler),
		vpp::descriptorBinding(vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &linearSampler),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &linearSampler),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &linearSampler),
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute),
	};

	dsLayout_ = {dev, taaBindings};
	vpp::nameHandle(dsLayout_, "TAAPass:dsLayout");

	ds_ = {dev.descriptorAllocator(), dsLayout_};
	vpp::nameHandle(ds_, "TAAPass:ds");

	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
	vpp::nameHandle(pipeLayout_, "TAAPass:pipeLayout");

	vpp::ShaderModule compShader(dev, taa_taa2_comp_data);
	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.module = compShader;
	cpi.stage.pName = "main";
	cpi.stage.stage = vk::ShaderStageBits::compute;

	pipe_ = {dev, cpi};
	vpp::nameHandle(pipe_, "TAAPass::pipe");

	auto uboSize = sizeof(float) * 2 + sizeof(params);
	ubo_ = {dev.bufferAllocator(), uboSize,
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

	// halton 16x
	constexpr auto len = 16;
	samples_.resize(len);

	// http://en.wikipedia.org/wiki/Halton_sequence
	// index not zero based
	auto halton = [](int prime, int index = 1){
		float r = 0.0f;
		float f = 1.0f;
		int i = index;
		while (i > 0) {
			f /= prime;
			r += f * (i % prime);
			i = std::floor(i / (float)prime);
		}
		return r;
	};

	// samples in range [-1, +1] per dimension, unit square
	for (auto i = 0; i < len; i++) {
		float u = 2 * (halton(2, i + 1) - 0.5f);
		float v = 2 * (halton(3, i + 1) - 0.5f);
		samples_[i] = {u, v};
	}
}

void TAAPass::initBuffers(vk::Extent2D size, vk::ImageView renderInput,
		vk::ImageView depthInput, vk::ImageView velInput) {
	auto& dev = dsLayout_.device();

	auto usage =
		vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::storage |
		vk::ImageUsageBits::transferDst |
		vk::ImageUsageBits::transferSrc;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, size, usage);
	info.img.arrayLayers = 2;
	dlg_assert(vpp::supported(dev, info.img));

	hist_ = {dev.devMemAllocator(), info.img, dev.deviceMemoryTypes()};
	vpp::nameHandle(hist_, "TAAPass:hist");

	info.view.image = hist_;
	info.view.subresourceRange.baseArrayLayer = 0;
	inHist_ = {dev, info.view};
	vpp::nameHandle(hist_, "TAAPass:inHist");

	info.view.subresourceRange.baseArrayLayer = 1;
	outHist_ = {dev, info.view};
	vpp::nameHandle(hist_, "TAAPass:outHist");

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler({{{}, inHist_, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.storage({{{}, outHist_, vk::ImageLayout::general}});
	dsu.imageSampler({{{}, renderInput, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.imageSampler({{{}, depthInput, vk::ImageLayout::depthStencilReadOnlyOptimal}});
	dsu.imageSampler({{{}, velInput, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.uniform({{{ubo_}}});
}

void TAAPass::record(vk::CommandBuffer cb, vk::Extent2D size) {
	// first make sure outHistory has the correct layout
	// we don't need the old contents so we can use image layout undefined
	vk::ImageMemoryBarrier obarrier;
	obarrier.image = hist_;
	obarrier.oldLayout = vk::ImageLayout::undefined;
	obarrier.newLayout = vk::ImageLayout::general;
	obarrier.dstAccessMask = vk::AccessBits::shaderWrite;
	obarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 1, 1};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{obarrier}});

	// run TAA shader
	auto cx = (size.width + 7) >> 3; // ceil(width / 8)
	auto cy = (size.height + 7) >> 3; // ceil(height / 8)
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {ds_});
	vk::cmdDispatch(cb, cx, cy, 1);

	// copy from outHistory back to inHistory for next frame
	// but we also use inHistory in the pp pass already
	// outHistory can be discarded after this
	vk::ImageMemoryBarrier ibarrier;
	ibarrier.image = hist_;
	ibarrier.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	ibarrier.srcAccessMask = vk::AccessBits::shaderRead;
	ibarrier.newLayout = vk::ImageLayout::transferDstOptimal;
	ibarrier.dstAccessMask = vk::AccessBits::transferWrite;
	ibarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};

	obarrier.oldLayout = vk::ImageLayout::general;
	obarrier.newLayout = vk::ImageLayout::transferSrcOptimal;
	obarrier.srcAccessMask = vk::AccessBits::shaderWrite;
	obarrier.dstAccessMask = vk::AccessBits::transferRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{ibarrier, obarrier}});

	vk::ImageCopy copy;
	copy.extent = {size.width, size.height, 1};
	copy.srcSubresource = {vk::ImageAspectBits::color, 0, 1, 1};
	copy.dstSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
	vk::cmdCopyImage(cb,
		hist_, vk::ImageLayout::transferSrcOptimal,
		hist_, vk::ImageLayout::transferDstOptimal,
		{{copy}});

	ibarrier.oldLayout = vk::ImageLayout::transferDstOptimal;
	ibarrier.srcAccessMask = vk::AccessBits::transferWrite;
	ibarrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	ibarrier.dstAccessMask = vk::AccessBits::shaderRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
		vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{ibarrier}});
}

void TAAPass::updateDevice(float near, float far) {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	tkn::write(span, near);
	tkn::write(span, far);
	tkn::write(span, params);
	map.flush();
}

nytl::Vec2f TAAPass::nextSample() {
	return samples_[++sampleID_ % samples_.size()];
}
