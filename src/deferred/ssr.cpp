#include "ssr.hpp"
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.ssr.comp.h>

void SSRPass::create(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;
	auto& dev = wb.dev;

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
			vk::ShaderStageBits::compute, -1, 1, &info.samplers.nearest),
		vpp::descriptorBinding( // normals
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &info.samplers.nearest),
		vpp::descriptorBinding( // output data
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // params ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute),
	};

	dsLayout_ = {dev, ssrBindings};
	pipeLayout_ = {dev, {{
		info.dsLayouts.scene.vkHandle(),
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

void SSRPass::createBuffers(InitBufferData& data, const doi::WorkBatcher& wb,
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
	dsu.imageSampler({{{}, ldepth,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.imageSampler({{{}, normals,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.storage({{{}, target_.vkImageView(),
		vk::ImageLayout::general}});
	dsu.uniform({{{ubo_}}});
}

RenderTarget SSRPass::record(vk::CommandBuffer cb, RenderTarget& depth,
		RenderTarget& normals, vk::DescriptorSet sceneDs, vk::Extent2D size) {
	vpp::DebugLabel debugLabel(cb, "SSRPass");
	transitionRead(cb, depth, vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::computeShader, vk::AccessBits::shaderRead);
	transitionRead(cb, normals, vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::computeShader, vk::AccessBits::shaderRead);

	RenderTarget ret;
	ret.image = target_.image();
	ret.view = target_.imageView();
	ret.layout = vk::ImageLayout::undefined;
	transitionWrite(cb, ret, vk::ImageLayout::general,
		vk::PipelineStageBits::computeShader, vk::AccessBits::shaderWrite);

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	doi::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {sceneDs, ds_});

	auto cx = u32(std::ceil(size.width / float(groupDimSize)));
	auto cy = u32(std::ceil(size.height / float(groupDimSize)));
	vk::cmdDispatch(cb, cx, cy, 1);
	return ret;
}

void SSRPass::updateDevice() {
	auto span = uboMap_.span();
	doi::write(span, params);
	uboMap_.flush();
}
