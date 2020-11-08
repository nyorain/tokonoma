#include "ssr.hpp"
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.ssr.comp.h>

// TODO: better blurred sampling. We could generate mipmaps of the light
// buffer (needed for exposure adaption anyways i guess) and then sample
// (and only slightly) blur from that

void SSRPass::create(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;
	auto& dev = wb.dev;
	ds_ = {};

	// work group size spec info
	ComputeGroupSizeSpec groupSizeSpec(groupDimSize, groupDimSize);

	// layouts
	// use nearest samplers since we run over a ray in screen space,
	// we will usually end up between two pixels but we want to
	// know one exact pixel that matches our requirements in the end.
	// TODO: optimizable, maybe we can work around potential
	// issues due to linear sampling? would probably be make binary
	// search more effictive i guess
	auto ssrBindings = {
		vpp::descriptorBinding( // linear depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		vpp::descriptorBinding( // normals
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		vpp::descriptorBinding( // output data
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // params ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute),
	};

	dsLayout_.init(dev, ssrBindings);
	pipeLayout_ = {dev, {{
		info.dsLayouts.camera.vkHandle(),
		dsLayout_.vkHandle()
	}}, {}};

	vpp::nameHandle(dsLayout_, "SSRPass:dsLayout_");
	vpp::nameHandle(pipeLayout_, "SSRPass:pipeLayout_");

	// pipe
	vpp::ShaderModule ssrShader(dev, deferred_ssr_comp_data);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.module = ssrShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
	pipe_ = {dev, cpi};
	vpp::nameHandle(pipe_, "SSRPass:pipe_");

	// ds & ubo
	ubo_ = {data.initUbo, wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
	ds_ = {data.initDs, wb.alloc.ds, dsLayout_};
}

void SSRPass::init(InitData& data, const PassCreateInfo&) {
	ubo_.init(data.initUbo);
	uboMap_ = ubo_.memoryMap();

	ds_.init(data.initDs);
	vpp::nameHandle(ds_, "SSRPass:ds_");
}

void SSRPass::createBuffers(InitBufferData& data, tkn::WorkBatcher& wb,
		vk::Extent2D size) {
	auto usage = vk::ImageUsageBits::storage |
		vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::inputAttachment;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, size, usage);
	dlg_assert(vpp::supported(wb.dev, info.img));
	target_ = {data.initTarget, wb.alloc.memDevice, info.img,
		wb.dev.deviceMemoryTypes()};
	data.viewInfo = info.view;
}

void SSRPass::initBuffers(InitBufferData& data, vk::ImageView ldepth,
		vk::ImageView normals) {
	target_.init(data.initTarget, data.viewInfo);
	vpp::nameHandle(target_.image(), "SSRPass:target_.image");
	vpp::nameHandle(target_.imageView(), "SSRPass:target_.imageView");

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler(ldepth);
	dsu.imageSampler(normals);
	dsu.storage(target_.vkImageView());
	dsu.uniform(ubo_);
}

void SSRPass::record(vk::CommandBuffer cb, vk::DescriptorSet sceneDs,
		vk::Extent2D size) {
	vpp::DebugLabel debugLabel(pipe_.device(), cb, "SSRPass");

	vk::ImageMemoryBarrier barrier;
	barrier.image = target_.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {sceneDs, ds_});

	auto cx = u32(std::ceil(size.width / float(groupDimSize)));
	auto cy = u32(std::ceil(size.height / float(groupDimSize)));
	vk::cmdDispatch(cb, cx, cy, 1);
}

void SSRPass::updateDevice() {
	auto span = uboMap_.span();
	tkn::write(span, params);
	uboMap_.flush();
}

SyncScope SSRPass::dstScopeNormals() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
}

SyncScope SSRPass::dstScopeDepth() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
}

SyncScope SSRPass::srcScopeTarget() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderWrite,
	};
}
