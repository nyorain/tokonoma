#include "ao.hpp"
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.ao.comp.h>

void AOPass::create(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;
	auto& dev = wb.dev;
	ds_ = {};

	auto aoBindings = std::array {
		vpp::descriptorBinding( // target
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // albedo
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		vpp::descriptorBinding( // ssao
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		vpp::descriptorBinding( // emission
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		vpp::descriptorBinding( // normal
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		vpp::descriptorBinding( // linear depth
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.nearest),
		vpp::descriptorBinding( // irradiance
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.linear),
		vpp::descriptorBinding( // envMap
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.linear),
		vpp::descriptorBinding( // brdflut
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &info.samplers.linear),
		vpp::descriptorBinding( // ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute),
	};

	dsLayout_.init(dev, aoBindings);
	ds_ = {data.initDs, wb.alloc.ds, dsLayout_};

	vk::PushConstantRange pcr;
	pcr.size = 4u;
	pcr.stageFlags = vk::ShaderStageBits::compute;
	pipeLayout_ = {dev, {{
		info.dsLayouts.camera.vkHandle(),
		dsLayout_.vkHandle()
	}}, {{pcr}}};

	vpp::nameHandle(dsLayout_, "AOPass:dsLayout_");
	vpp::nameHandle(pipeLayout_, "AOPass:pipeLayout_");

	// pipe
	ComputeGroupSizeSpec groupSizeSpec(groupDimSize, groupDimSize);
	vpp::ShaderModule aoShader(dev, deferred_ao_comp_data);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.module = aoShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
	pipe_ = {dev, cpi};
	vpp::nameHandle(pipe_, "AOPass:pipe_");

	// ubo
	ubo_ = {data.initUbo, wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
}

void AOPass::init(InitData& data, const PassCreateInfo&) {
	ubo_.init(data.initUbo);
	uboMap_ = ubo_.memoryMap();

	ds_.init(data.initDs);
	vpp::nameHandle(ds_, "AOPass:ds_");
}

void AOPass::updateInputs(vk::ImageView light,
		vk::ImageView albeo, vk::ImageView emission, vk::ImageView ldepth,
		vk::ImageView normal, vk::ImageView ssao,
		vk::ImageView irradiance, vk::ImageView filteredEnv,
		unsigned filteredEnvLods, vk::ImageView brdflut) {
	envFilterLods_ = filteredEnvLods;

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.storage(light);
	dsu.imageSampler(albeo);
	dsu.imageSampler(emission);
	dsu.imageSampler(ldepth);
	dsu.imageSampler(normal);
	dsu.imageSampler(ssao);
	dsu.imageSampler(irradiance);
	dsu.imageSampler(filteredEnv);
	dsu.imageSampler(brdflut);
	dsu.uniform(ubo_);
}

void AOPass::record(vk::CommandBuffer cb, vk::DescriptorSet sceneDs,
		vk::Extent2D size) {
	vpp::DebugLabel debugLabel(pipe_.device(), cb, "AOPass");
	dlg_assert(envFilterLods_);

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {sceneDs, ds_});
	vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute,
		0, 4, &envFilterLods_);
	auto cx = std::ceil(size.width / float(groupDimSize));
	auto cy = std::ceil(size.height / float(groupDimSize));
	vk::cmdDispatch(cb, cx, cy, 1);
}

void AOPass::updateDevice() {
	auto span = uboMap_.span();
	tkn::write(span, params);
	uboMap_.flush();
}

SyncScope AOPass::scopeLight() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite
	};
}
SyncScope AOPass::dstScopeSSAO() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
}
SyncScope AOPass::dstScopeGBuf() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
}
