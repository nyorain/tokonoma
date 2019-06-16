#include "ssao.hpp"
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <nytl/vecOps.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.ssao.frag.h>
#include <shaders/deferred.ssaoBlur.frag.h>
#include <shaders/deferred.ssao.comp.h>
#include <shaders/deferred.ssaoBlur.comp.h>

#include <random>

// NOTE: when using the compute shader, we could read from readonly
// storage images in the blur shaders, right? no need for sampling there

namespace {
vpp::ViewableImageCreateInfo targetInfo(vk::Extent2D size, bool compute) {
	auto usage = nytl::Flags(vk::ImageUsageBits::sampled);
	if(compute) {
		usage |= vk::ImageUsageBits::storage;
	} else {
		usage |= vk::ImageUsageBits::colorAttachment;
	}

	return {SSAOPass::format, vk::ImageAspectBits::color, size, usage};
}
} // anon namespace

void SSAOPass::create(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;
	auto& dev = wb.dev;

	// test if r8Unorm can be used as storage image. Supported on pretty
	// much all desktop systems.
	// we use a dummy size here but that shouldn't matter
	auto compute = vpp::supported(dev, targetInfo({1024, 1024}, true).img);
	auto stage = vk::ShaderStageBits::compute;
	if(!compute) {
		dlg_info("Can't use compute pipeline for SSAO");
		stage = vk::ShaderStageBits::fragment;

		// render pass
		vk::AttachmentDescription attachment;
		attachment.format = format;
		attachment.samples = vk::SampleCountBits::e1;
		attachment.loadOp = vk::AttachmentLoadOp::dontCare;
		attachment.storeOp = vk::AttachmentStoreOp::store;
		attachment.stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachment.stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachment.initialLayout = vk::ImageLayout::undefined;
		attachment.finalLayout = vk::ImageLayout::shaderReadOnlyOptimal;

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

		rp_ = {dev, rpi};
		vpp::nameHandle(rp_, "SSAOPass:rp_");
	}

	// pipeline
	auto ssaoBindings = std::vector {
		vpp::descriptorBinding( // ssao samples and such
			vk::DescriptorType::uniformBuffer, stage),
		// nearest sampler since we always sample pixel center and
		// don't want to interpolate the random sample vectors
		vpp::descriptorBinding( // ssao noise texture
			vk::DescriptorType::combinedImageSampler,
			stage, -1, 1, &info.samplers.nearest),
		// linear sampler since we sapmle depth randomly
		vpp::descriptorBinding( // depth texture
			vk::DescriptorType::combinedImageSampler,
			stage, -1, 1, &info.samplers.linear),
		// nearest sampler since we only sample the pixel center
		vpp::descriptorBinding( // normals texture
			vk::DescriptorType::combinedImageSampler,
			stage, -1, 1, &info.samplers.nearest),
	};

	auto blurBindings = std::vector {
		// nearest samplers since we always sample at pixel center
		// ssao (or previous ssao blur)
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			stage, -1, 1, &info.samplers.nearest),
		// normals
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			stage, -1, 1, &info.samplers.nearest),
	};

	if(compute) {
		// output image
		ssaoBindings.push_back(vpp::descriptorBinding(
			vk::DescriptorType::storageImage, stage));
		blurBindings.push_back(vpp::descriptorBinding(
			vk::DescriptorType::storageImage, stage));
	}

	dsLayout_ = {dev, ssaoBindings};
	pipeLayout_ = {dev, {{
		info.dsLayouts.camera.vkHandle(),
		dsLayout_.vkHandle()
	}}, {}};

	blur_.dsLayout = {dev, blurBindings};
	vk::PushConstantRange blurPcr;
	blurPcr.size = 4;
	blurPcr.stageFlags = stage;
	blur_.pipeLayout = {dev, {{blur_.dsLayout.vkHandle()}}, {{blurPcr}}};

	vpp::nameHandle(dsLayout_, "SSAOPass:dsLayout_");
	vpp::nameHandle(pipeLayout_, "SSAOPass:pipeLayout_");
	vpp::nameHandle(blur_.dsLayout, "SSAOPass:blur_.dsLayout");
	vpp::nameHandle(blur_.pipeLayout, "SSAOPass:blur_pipeLayout");

	if(compute) {
		std::array<vk::SpecializationMapEntry, 3> entries = {{
			{0, 0, 4u},
			{1, 4u, 4u},
			{2, 8u, 4u},
		}};

		std::array<u32, 3> data = {sampleCount, groupDimSize, groupDimSize};

		vk::SpecializationInfo spi;
		spi.mapEntryCount = entries.size();
		spi.pMapEntries = entries.data();
		spi.dataSize = data.size() * sizeof(data[0]);
		spi.pData = data.data();

		vpp::ShaderModule ssaoShader(dev, deferred_ssao_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = pipeLayout_;
		cpi.stage.module = ssaoShader;
		cpi.stage.stage = vk::ShaderStageBits::compute;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &spi;
		pipe_ = {dev, cpi};

		// blur
		std::array<vk::SpecializationMapEntry, 2> blurEntries = {{
			{0, 0, 4u},
			{1, 4u, 4u},
		}};
		std::array<u32, 2> blurData = {groupDimSize, groupDimSize};

		spi.mapEntryCount = blurEntries.size();
		spi.pMapEntries = blurEntries.data();
		spi.dataSize = blurData.size() * sizeof(blurData[0]);
		spi.pData = blurData.data();

		vpp::ShaderModule blurShader(dev, deferred_ssaoBlur_comp_data);
		cpi.layout = blur_.pipeLayout;
		cpi.stage.module = blurShader;
		cpi.stage.stage = vk::ShaderStageBits::compute;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &spi;
		blur_.pipe = {dev, cpi};
	} else {
		vk::SpecializationMapEntry entry;
		entry.constantID = 0u;
		entry.offset = 0u;
		entry.size = 4u;

		auto data = u32(sampleCount);
		vk::SpecializationInfo spi;
		spi.mapEntryCount = 1;
		spi.pMapEntries = &entry;
		spi.dataSize = sizeof(data);
		spi.pData = &data;

		vpp::ShaderModule ssaoShader(dev, deferred_ssao_frag_data);
		vpp::GraphicsPipelineInfo gpi {rp_, pipeLayout_, {{{
			{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
			{ssaoShader, vk::ShaderStageBits::fragment, &spi},
		}}}};

		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.attachmentCount = 1u;
		gpi.blend.pAttachments = &doi::noBlendAttachment();

		pipe_ = {dev, gpi.info()};

		// blur
		vpp::ShaderModule blurShader(dev, deferred_ssaoBlur_frag_data);
		gpi  = {rp_, blur_.pipeLayout, {{{
			{info.fullscreenVertShader, vk::ShaderStageBits::vertex},
			{blurShader, vk::ShaderStageBits::fragment},
		}}}};

		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.attachmentCount = 1u;
		gpi.blend.pAttachments = &doi::noBlendAttachment();

		blur_.pipe = {dev, gpi.info()};
	}

	vpp::nameHandle(pipe_, "SSAOPass:pipe_");

	// ssao data
	std::default_random_engine rndEngine;
	std::uniform_real_distribution<float> rndDist(0.0f, 1.0f);

	// Sample kernel
	std::vector<nytl::Vec4f> kernel(sampleCount);
	for(auto i = 0u; i < sampleCount; ++i) {
		nytl::Vec3f sample{
			2.f * rndDist(rndEngine) - 1.f,
			2.f * rndDist(rndEngine) - 1.f,
			rndDist(rndEngine)};
		sample = normalized(sample);
		sample *= rndDist(rndEngine);
		float scale = float(i) / float(sampleCount);
		scale = nytl::mix(0.1f, 1.0f, scale * scale);
		kernel[i] = nytl::Vec4f(scale * sample);
	}

	// sort for better cache coherency on the gpu
	// not sure if that really effects something but it shouldn't hurt.
	// sorting not optimal for every angle obviously, more of a guess/
	// try here
	auto sorter = [](auto& vec1, auto& vec2) {
		return std::atan2(vec1[1], vec1[0]) < std::atan2(vec2[1], vec2[0]);
	};
	std::sort(kernel.begin(), kernel.end(), sorter);
	data.samples = std::move(kernel);

	auto devMem = dev.deviceMemoryTypes();
	auto size = sizeof(nytl::Vec4f) * sampleCount;
	auto usage = vk::BufferUsageBits::transferDst |
		vk::BufferUsageBits::uniformBuffer;
	samples_ = {data.initSamples, wb.alloc.bufDevice, size, usage, devMem};
	data.samplesStage = {data.initSamplesStage, wb.alloc.bufStage,
		size, vk::BufferUsageBits::transferSrc, dev.hostMemoryTypes()};

	// NOTE: we could use a r32g32f format, would be more efficent
	// might not be supported though... we could pack it into somehow
	// TODO: or simply use a buffer/constant array in shader? no advantage
	// like this. We could make noiseDim here variable/configurable though.
	constexpr auto noiseDim = 4u;
	std::vector<nytl::Vec4f> noiseData;
	noiseData.resize(noiseDim * noiseDim);
	for(auto i = 0u; i < noiseDim * noiseDim; i++) {
		noiseData[i] = nytl::Vec4f{
			rndDist(rndEngine) * 2.f - 1.f,
			rndDist(rndEngine) * 2.f - 1.f,
			0.0f, 0.0f
		};
	}

	auto noiseFormat = vk::Format::r32g32b32a32Sfloat;
	auto span = nytl::as_bytes(nytl::span(noiseData));
	auto p = doi::wrap({noiseDim, noiseDim}, noiseFormat, span);
	auto params = doi::TextureCreateParams{};
	params.format = noiseFormat;
	noise_ = {data.initNoise, wb, std::move(p), params};

	ds_ = {data.initDs, wb.alloc.ds, dsLayout_};
	blur_.dsHorz = {data.initBlurHDs, wb.alloc.ds, blur_.dsLayout};
	blur_.dsVert = {data.initBlurVDs, wb.alloc.ds, blur_.dsLayout};
}

void SSAOPass::init(InitData& data, const PassCreateInfo& info) {
	auto& wb = info.wb;

	ds_.init(data.initDs);
	blur_.dsHorz.init(data.initBlurHDs);
	blur_.dsVert.init(data.initBlurVDs);
	noise_.init(data.initNoise, wb);
	samples_.init(data.initSamples);

	vpp::nameHandle(ds_, "SSAOPass:ds_");
	vpp::nameHandle(blur_.dsHorz, "SSAOPass:blur_.dsHorz");
	vpp::nameHandle(blur_.dsVert, "SSAOPass:blur_.dsVert");
	vpp::nameHandle(noise_.image(), "SSAOPass:noise_.image");
	vpp::nameHandle(noise_.imageView(), "SSAOPass:noise_.imageView");

	data.samplesStage.init(data.initSamplesStage);
	auto map = data.samplesStage.memoryMap();
	std::memcpy(map.ptr(), data.samples.data(), data.samplesStage.size());
	map = {};

	vk::BufferCopy copy;
	copy.srcOffset = data.samplesStage.offset();
	copy.dstOffset = samples_.offset();
	copy.size = samples_.size();
	vk::cmdCopyBuffer(wb.cb, data.samplesStage.buffer(), samples_.buffer(),
		{{copy}});
}

void SSAOPass::createBuffers(InitBufferData& data, const doi::WorkBatcher& wb,
		vk::Extent2D size) {
	auto info = targetInfo(size, usingCompute());
	dlg_assert(vpp::supported(wb.dev, info.img));
	data.viewInfo = info.view;
	target_ = {data.initTarget, wb.alloc.memDevice, info.img,
		wb.dev.deviceMemoryTypes()};
	blur_.target = {data.initBlurTarget, wb.alloc.memDevice, info.img,
		wb.dev.deviceMemoryTypes()};
}

void SSAOPass::initBuffers(InitBufferData& data, vk::ImageView depth,
		vk::ImageView normals, vk::Extent2D size) {
	target_.init(data.initTarget, data.viewInfo);
	blur_.target.init(data.initBlurTarget, data.viewInfo);
	vpp::nameHandle(target_.image(), "SSAOPass:target_.image");
	vpp::nameHandle(target_.imageView(), "SSAOPass:target_.imageView");
	vpp::nameHandle(blur_.target.image(), "SSAOPass:blur_.target.image");
	vpp::nameHandle(blur_.target.imageView(), "SSAOPass:blur_.target.imageView");

	if(!usingCompute()) {
		vk::FramebufferCreateInfo fbi;
		fbi.renderPass = rp_;
		fbi.width = size.width;
		fbi.height = size.height;
		fbi.layers = 1;

		auto attachments = {target_.vkImageView()};
		fbi.attachmentCount = attachments.size();
		fbi.pAttachments = attachments.begin();
		fb_ = {target_.device(), fbi};

		auto blurAttachments = {blur_.target.vkImageView()};
		fbi.attachmentCount = blurAttachments.size();
		fbi.pAttachments = blurAttachments.begin();
		blur_.fb = {target_.device(), fbi};
	}

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.uniform({{{samples_}}});
	dsu.imageSampler({{{}, noise_.imageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.imageSampler({{{}, depth, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.imageSampler({{{}, normals, vk::ImageLayout::shaderReadOnlyOptimal}});

	vpp::DescriptorSetUpdate hdsu(blur_.dsHorz);
	hdsu.imageSampler({{{}, target_.imageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	hdsu.imageSampler({{{}, depth, vk::ImageLayout::shaderReadOnlyOptimal}});

	vpp::DescriptorSetUpdate vdsu(blur_.dsVert);
	vdsu.imageSampler({{{}, blur_.target.imageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	vdsu.imageSampler({{{}, depth, vk::ImageLayout::shaderReadOnlyOptimal}});

	if(usingCompute()) {
		dsu.storage({{{}, target_.vkImageView(), vk::ImageLayout::general}});
		hdsu.storage({{{}, blur_.target.vkImageView(), vk::ImageLayout::general}});
		vdsu.storage({{{}, target_.vkImageView(), vk::ImageLayout::general}});
	}

	vpp::apply({{{dsu}, {hdsu}, {vdsu}}});
}

void SSAOPass::record(vk::CommandBuffer cb,
		vk::Extent2D size, vk::DescriptorSet sceneDs) {
	vpp::DebugLabel debugLabel(cb, "SSAOPass");
	auto width = size.width;
	auto height = size.height;

	if(usingCompute()) {
		u32 dx = std::ceil(width / float(groupDimSize));
		u32 dy = std::ceil(height / float(groupDimSize));

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
		doi::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {sceneDs, ds_});
		vk::cmdDispatch(cb, dx, dy, 1);

		// make sure write to target_ is visible since it will be
		// read in next pass (blur)
		barrier.image = target_.image();
		barrier.oldLayout = vk::ImageLayout::general;
		barrier.srcAccessMask = vk::AccessBits::shaderWrite;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};

		// also make sure that the blur target is in general layout
		vk::ImageMemoryBarrier blurBarrier;
		blurBarrier.image = blur_.target.image();
		blurBarrier.oldLayout = vk::ImageLayout::undefined; // will be overwritten
		blurBarrier.srcAccessMask = vk::AccessBits::shaderWrite;
		blurBarrier.newLayout = vk::ImageLayout::general;
		blurBarrier.dstAccessMask = vk::AccessBits::shaderWrite;
		blurBarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier, blurBarrier}});

		// blurring
		u32 horizontal = 1u;
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, blur_.pipe);
		vk::cmdPushConstants(cb, blur_.pipeLayout,
			vk::ShaderStageBits::compute, 0, 4, &horizontal);
		doi::cmdBindComputeDescriptors(cb, blur_.pipeLayout, 0, {blur_.dsHorz});
		vk::cmdDispatch(cb, dx, dy, 1);

		// i guess when we make sure that write to blur_.target has finished,
		// reading from target_ must logically have finished as well
		// layout transition needed here though
		barrier.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessBits::shaderRead;
		barrier.newLayout = vk::ImageLayout::general;
		barrier.dstAccessMask = vk::AccessBits::shaderWrite;

		// make sure write to blur_.target is visible since it will be
		// written in next blur pass
		blurBarrier.oldLayout = vk::ImageLayout::general;
		blurBarrier.srcAccessMask = vk::AccessBits::shaderWrite;
		blurBarrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		blurBarrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{barrier, blurBarrier}});

		horizontal = 0u;
		vk::cmdPushConstants(cb, blur_.pipeLayout,
			vk::ShaderStageBits::compute, 0, 4, &horizontal);
		doi::cmdBindComputeDescriptors(cb, blur_.pipeLayout, 0, {blur_.dsVert});
		vk::cmdDispatch(cb, dx, dy, 1);
	} else {
		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

		vk::cmdBeginRenderPass(cb, {rp_, fb_,
			{0u, 0u, width, height}, 0, nullptr}, {});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {sceneDs, ds_});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);

		// make sure write to target_ is visible since it will be
		// read in next pass (blur)
		vk::ImageMemoryBarrier barrier;
		barrier.image = target_.image();
		barrier.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::colorAttachmentOutput,
			vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{barrier}});

		// blurring
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics,
			blur_.pipe);

		// horizontal
		vk::cmdBeginRenderPass(cb, {rp_, blur_.fb,
			{0u, 0u, width, height}, 0, nullptr}, {});
		u32 horizontal = 1u;
		vk::cmdPushConstants(cb, blur_.pipeLayout,
			vk::ShaderStageBits::fragment, 0, 4, &horizontal);
		doi::cmdBindGraphicsDescriptors(cb, blur_.pipeLayout, 0, {blur_.dsHorz});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);

		// make sure write to blur_.target is visible since it will be
		// written in next blur pass
		barrier.image = blur_.target.image();
		barrier.oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.srcAccessMask = vk::AccessBits::colorAttachmentWrite;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::colorAttachmentOutput,
			vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{barrier}});

		// vertical
		horizontal = 0u;
		vk::cmdBeginRenderPass(cb, {rp_, fb_,
			{0u, 0u, width, height}, 0, nullptr}, {});
		vk::cmdPushConstants(cb, blur_.pipeLayout,
			vk::ShaderStageBits::fragment, 0, 4, &horizontal);
		doi::cmdBindGraphicsDescriptors(cb, blur_.pipeLayout, 0, {blur_.dsVert});
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen
		vk::cmdEndRenderPass(cb);
	}
}

SyncScope SSAOPass::dstScopeDepth() const {
	return {
		usingCompute() ?
			vk::PipelineStageBits::computeShader :
			vk::PipelineStageBits::fragmentShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
}

SyncScope SSAOPass::dstScopeNormals() const {
	return {
		usingCompute() ?
			vk::PipelineStageBits::computeShader :
			vk::PipelineStageBits::fragmentShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
}

SyncScope SSAOPass::srcScopeTarget() const {
	if(usingCompute()) {
		return {
			vk::PipelineStageBits::computeShader,
			vk::ImageLayout::general,
			vk::AccessBits::shaderWrite,
		};
	} else {
		return {
			vk::PipelineStageBits::colorAttachmentOutput,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::AccessBits::colorAttachmentWrite,
		};
	}
}
