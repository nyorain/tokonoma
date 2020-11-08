#include "combine.hpp"
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.combine.comp.h>

void CombinePass::create(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;
	auto& dev = wb.dev;
	ds_ = {};

	auto combineBindings = std::array {
		// output
		vpp::descriptorBinding(
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		// we use the nearest sampler here since we use it for fxaa and ssr
		// and for ssr we *need* nearest (otherwise be bleed artefacts).
		// Not sure about fxaa but seems to work with nearest.
		vpp::descriptorBinding( // output from combine pass
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		// we sample per pixel, nearest should be alright
		vpp::descriptorBinding( // ssr output
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		// here it's important to use a linear sampler since we upscale
		vpp::descriptorBinding( // bloom
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.linear),
		// depth (for ssr), use nearest sampler as ssr does
		vpp::descriptorBinding( // depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		// light scattering, no guassian blur atm, could use either sampler i
		// guess?
		vpp::descriptorBinding( // light scattering
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.linear),
		vpp::descriptorBinding( // params ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute),
	};

	dsLayout_.init(dev, combineBindings);
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
	vpp::nameHandle(dsLayout_, "CombinePass:dsLayout_");
	vpp::nameHandle(pipeLayout_, "CombinePass:pipeLayout_");

	vpp::ShaderModule combineShader(dev, deferred_combine_comp_data);
	ComputeGroupSizeSpec groupSizeSpec(groupDimSize, groupDimSize);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.module = combineShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
	pipe_ = {dev, cpi};
	vpp::nameHandle(pipe_, "CombinePass:pipe_");

	ds_ = {data.initDs, wb.alloc.ds, dsLayout_};
	ubo_ = {data.initUbo, wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
}

void CombinePass::init(InitData& data, const PassCreateInfo&) {
	ubo_.init(data.initUbo);
	uboMap_ = ubo_.memoryMap();
	ds_.init(data.initDs);
	vpp::nameHandle(ds_, "CombinePass:ds_");
}

void CombinePass::updateInputs(vk::ImageView output, vk::ImageView light,
		vk::ImageView ldepth, vk::ImageView bloom,
		vk::ImageView ssr, vk::ImageView scattering) {
	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.storage(output);
	dsu.imageSampler(light);
	dsu.imageSampler(ldepth);
	dsu.imageSampler(bloom);
	dsu.imageSampler(ssr);
	dsu.imageSampler(scattering);
	dsu.uniform(ubo_);
}

void CombinePass::record(vk::CommandBuffer cb, vk::Extent2D size) {
	vpp::DebugLabel(pipe_.device(), cb, "CombinePass");
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {ds_});
	auto cx = std::ceil(size.width / float(groupDimSize));
	auto cy = std::ceil(size.height / float(groupDimSize));
	vk::cmdDispatch(cb, cx, cy, 1);
}

void CombinePass::updateDevice() {
	auto span = uboMap_.span();
	tkn::write(span, params);
	uboMap_.flush();
}

SyncScope CombinePass::dstScopeBloom() const {
	return SyncScope::computeSampled();
}
SyncScope CombinePass::dstScopeSSR() const {
	return SyncScope::computeSampled();
}
SyncScope CombinePass::dstScopeLight() const {
	return SyncScope::computeSampled();
}
SyncScope CombinePass::dstScopeDepth() const {
	return SyncScope::computeSampled();
}
SyncScope CombinePass::dstScopeScatter() const {
	return SyncScope::computeSampled();
}
SyncScope CombinePass::scopeTarget() const {
	// TODO: discarding scope, no way to signal that at the moment
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderWrite,
	};
}
