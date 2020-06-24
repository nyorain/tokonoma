#include <tkn/passes/highlight.hpp>
#include <vpp/debug.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>

#include <shaders/deferred.highlight.comp.h>

namespace tkn {

// HighLightPass
void HighLightPass::create(InitData& data, WorkBatcher& wb, vk::Sampler linear) {
	auto& dev = wb.dev;
	ComputeGroupSizeSpec groupSizeSpec(groupDimSize, groupDimSize);
	ds_ = {};

	auto bindings = std::array {
		// light & emission
		// We need a linear filter since we downscale
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &linear),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &linear),
		// output color
		vpp::descriptorBinding(
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		// ubo params
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute),
	};

	dsLayout_.init(dev, bindings);
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
	vpp::nameHandle(dsLayout_, "HighLightPass:dsLayout");
	vpp::nameHandle(pipeLayout_, "HighLightPass:pipeLayout");

	vpp::ShaderModule shader(dev, deferred_highlight_comp_data);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.module = shader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
	pipe_ = {dev, cpi};
	vpp::nameHandle(pipe_, "HightLightPass:pipe");

	ds_ = {data.initDs, wb.alloc.ds, dsLayout_};
	ubo_ = {data.initUbo, wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
}

void HighLightPass::init(InitData& data) {
	ds_.init(data.initDs);
	ubo_.init(data.initUbo);
	vpp::nameHandle(ds_, "HighLightPass:ds");
}

void HighLightPass::createBuffers(InitBufferData& data,
		tkn::WorkBatcher& wb, vk::Extent2D size) {
	auto& dev = wb.dev;
	size.width = std::max(size.width >> 1, 1u);
	size.height = std::max(size.height >> 1, 1u);

	// TODO: make usage a public parameter like numLevels?
	// strictly speaking *we* don't need transfer usages
	auto usage = vk::ImageUsageBits::storage |
		vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst;
	auto info = vpp::ViewableImageCreateInfo(vk::Format::r16g16b16a16Sfloat,
		vk::ImageAspectBits::color, size, usage);
	dlg_assert(vpp::supported(dev, info.img));

	dlg_assert(numLevels >= 1);
	dlg_assert((2u << numLevels) <= size.width ||
		(2u << numLevels) <= size.height);

	info.img.extent.depth = 1;
	info.img.mipLevels = numLevels;
	target_ = {data.initTarget, wb.alloc.memDevice, info.img,
		dev.deviceMemoryTypes()};
	data.viewInfo = info.view;
}

void HighLightPass::initBuffers(InitBufferData& data, vk::ImageView lightInput,
		vk::ImageView emissionInput) {
	target_.init(data.initTarget, data.viewInfo);
	vpp::nameHandle(target_, "HightLightPass:target");

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler({{{}, lightInput,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.imageSampler({{{}, emissionInput,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.storage({{{}, target_.vkImageView(),
		vk::ImageLayout::general}});
	dsu.uniform({{{ubo_}}});
}

void HighLightPass::record(vk::CommandBuffer cb, vk::Extent2D size) {
	vpp::DebugLabel(target_.device(), cb, "HighlightPass");

	vk::ImageMemoryBarrier barrier;
	barrier.image = target_.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

	auto gx = (std::max(size.width >> 1, 1u) + groupDimSize - 1) / groupDimSize;
	auto gy = (std::max(size.height >> 1, 1u) + groupDimSize - 1) / groupDimSize;

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {{ds_}});
	vk::cmdDispatch(cb, gx, gy, 1);
}

void HighLightPass::updateDevice() {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	tkn::write(span, params);
	map.flush();
}

SyncScope HighLightPass::dstScopeEmission() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
}

SyncScope HighLightPass::dstScopeLight() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
}

SyncScope HighLightPass::srcScopeTarget() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderWrite
	};
}

} // namespace tkn
