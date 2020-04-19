#include "lens.hpp"
#include <tkn/bits.hpp>
#include <tkn/render.hpp>
#include <vpp/debug.hpp>
#include <vpp/shader.hpp>
#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <dlg/dlg.hpp>

#include <shaders/deferred.gblur.comp.h>
#include <shaders/deferred.highlight.comp.h>
#include <shaders/deferred.lens.comp.h>

// utility
namespace {

// Returns a row in pascal's triangle, starting at 1. This means
// row r will have r coefficients.
std::vector<unsigned> pascal(unsigned row) {
	std::vector<unsigned> ret;
	ret.reserve(row);

	unsigned last = 1;
	ret.push_back(last);

	for(auto i = 1u; i < row; ++i) {
		last = last * (row - i) / i;
		ret.push_back(last);
	}

	return ret;
}

// Might only works for positive numbers
float roundOdd(float x) {
	return std::floor(x / 2) * 2 + 1;
}

} // anon namespace

// GaussianBlur
GaussianBlur::Kernel GaussianBlur::createKernel(unsigned int hsize,
		float fac) {
	dlg_assert(hsize > 1 && hsize < 31);
	dlg_assert(fac >= 1.0);

	// this is important so we can divide hsize by 2 later on
	// to merge the weights and offsets
	if(hsize % 2) {
		++hsize;
	}

	auto ksize = 1 + hsize * 2; // full kernel size
	auto psize = unsigned(roundOdd(ksize * fac));
	auto prow = pascal(psize);
	dlg_assert(prow.size() == psize);

	// sum up all relevant parts of the pascal row
	// we use the symmetry of the pascal row here (we know that
	// the number of values in the row is odd)
	auto start = (psize - 1) / 2;
	float sum = prow[start];
	for(auto i = start + 1; i < start + hsize + 1; ++i) {
		sum += 2 * prow[i];
	}

	// Can be undefined except for the values we write
	std::array<Vec2f, 16> kernel;
	kernel[0] = {float(1 + hsize / 2), prow[start] / sum};
	auto idx = 0u;

	for(auto i = start + 1; i < start + hsize + 1; i += 2) {
		auto w1 = prow[i + 0] / sum;
		auto w2 = prow[i + 1] / sum;
		auto w = w1 + w2;
		auto o1 = i - start - 1;
		auto o2 = i - start;
		float o = (o1 * w1 + o2 * w2) / w;
		kernel[++idx] = {o, w};
	}

	dlg_assert(idx < 16);
	return kernel;
}

void GaussianBlur::init(const vpp::Device& dev, vk::Sampler linearSampler) {
	auto bindings = {
		// important to use linear sampler here, our shader is optimized,
		// reads multiple pixels in a single fetch via linear sampling
		vpp::descriptorBinding( // input color
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &linearSampler),
		vpp::descriptorBinding( // output color, blurred
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute)
	};

	vk::PushConstantRange pcr;
	pcr.size = 128;
	pcr.stageFlags = vk::ShaderStageBits::compute;

	dsLayout_ = {dev, bindings};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};
	vpp::nameHandle(dsLayout_, "GaussianBlur:dsLayout");
	vpp::nameHandle(pipeLayout_, "GaussianBlur:pipeLayout");

	// pipe
	vpp::ShaderModule blurShader(dev, deferred_gblur_comp_data);
	tkn::ComputeGroupSizeSpec groupSizeSpec(groupDimSize, groupDimSize);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.module = blurShader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
	pipe_ = {dev, cpi};
	vpp::nameHandle(pipe_, "GaussianBlur:pipe");
}

GaussianBlur::Instance GaussianBlur::createInstance(
		InstanceInitData& data, vpp::DescriptorAllocator* alloc) const {
	alloc = alloc ? alloc : &pipeLayout_.device().descriptorAllocator();
	return Instance{
		vpp::TrDs{data.ping, *alloc, dsLayout_},
		vpp::TrDs{data.pong, *alloc, dsLayout_}
	};
}

void GaussianBlur::initInstance(Instance& ini, InstanceInitData& data) const {
	ini.ping.init(data.ping);
	ini.pong.init(data.pong);
	vpp::nameHandle(ini.ping, "GaussianBlur:ping");
	vpp::nameHandle(ini.ping, "GaussianBlur:pong");
}

void GaussianBlur::updateInstance(Instance& ds, vk::ImageView view,
		vk::ImageView tmp) const {
	vpp::DescriptorSetUpdate dsu1(ds.ping);
	dsu1.imageSampler({{{}, view, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu1.storage({{{}, tmp, vk::ImageLayout::general}});

	vpp::DescriptorSetUpdate dsu2(ds.pong);
	dsu2.imageSampler({{{}, tmp, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu2.storage({{{}, view, vk::ImageLayout::general}});
}

void GaussianBlur::record(vk::CommandBuffer cb, const Instance& instance,
		const vk::Extent2D& dstSize, Image srcDst, Image tmp,
		const Kernel& kernel, vk::ImageAspectBits aspect) const {
	dlg_assert(dstSize.width > 0 && dstSize.height > 0);
	dlg_assert(kernel[0].x > 1);
	vpp::DebugLabel(instance.ping.device(), cb, "GaussianBlur");

	// basically ceil(dstSize / float(groupDimSize))
	auto gx = (dstSize.width + groupDimSize - 1) / groupDimSize;
	auto gy = (dstSize.height + groupDimSize - 1) / groupDimSize;

	// horizontal pass
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute, 0,
		128, kernel.data());
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {instance.ping});
	vk::cmdDispatch(cb, gx, gy, 1);

	// make sure read from srcDst is finish before writing to it
	// we overwrite its old contents, therefore use vk::ImageLayout::undefined
	vk::ImageMemoryBarrier barrier;
	barrier.image = srcDst.image;
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = vk::AccessBits::shaderRead;
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange = {aspect, srcDst.mipLevel, 1, srcDst.arrayLayer, 1};

	// make sure writing to tmp is finished before reading from it
	vk::ImageMemoryBarrier btmp;
	btmp.image = tmp.image;
	btmp.oldLayout = vk::ImageLayout::general;
	btmp.srcAccessMask = vk::AccessBits::shaderWrite;
	btmp.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	btmp.dstAccessMask = vk::AccessBits::shaderRead;
	btmp.subresourceRange = {aspect, tmp.mipLevel, 1, tmp.arrayLayer, 1};
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::computeShader,
		{}, {}, {}, {{barrier, btmp}});

	// vertical pass
	// we only have to update the first value of the kernel to signal
	// that this is the vertical pass
	float nx = -kernel[0].x;
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute, 0,
		4, &nx);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {instance.pong});
	vk::cmdDispatch(cb, gx, gy, 1);
}

SyncScope GaussianBlur::dstScope() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead
	};
}

SyncScope GaussianBlur::srcScope() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderWrite
	};
}

SyncScope GaussianBlur::dstScopeTmp() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderWrite
	};
}

SyncScope GaussianBlur::srcScopeTmp() const {
	return {
		vk::PipelineStageBits::computeShader,
		// We use undefined here because the contents are undefined
		vk::ImageLayout::undefined,
		vk::AccessBits::shaderWrite
	};
}

// HighLightPass
void HighLightPass::create(InitData& data, const PassCreateInfo& pci) {
	auto& dev = pci.wb.dev;
	ComputeGroupSizeSpec groupSizeSpec(groupDimSize, groupDimSize);

	auto bindings = {
		// light & emission
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &pci.samplers.nearest),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &pci.samplers.nearest),
		// output color
		vpp::descriptorBinding(
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		// ubo params
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute),
	};

	dsLayout_ = {dev, bindings};
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

	ds_ = {data.initDs, pci.wb.alloc.ds, dsLayout_};
	ubo_ = {data.initUbo, pci.wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
}

void HighLightPass::init(InitData& data, const PassCreateInfo&) {
	ds_.init(data.initDs);
	ubo_.init(data.initUbo);
	vpp::nameHandle(ds_, "HighLightPass:ds");
}

void HighLightPass::createBuffers(InitBufferData& data,
		const tkn::WorkBatcher& wb, vk::Extent2D size) {
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

// Lens
void LensFlare::create(InitData& data, const PassCreateInfo& pci,
		const GaussianBlur& blur) {
	auto& dev = pci.wb.dev;

	auto bindings = {
		vpp::descriptorBinding( // highlight input
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &pci.samplers.linear),
		vpp::descriptorBinding( // output color
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding( // ubo
			vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute)
	};

	dsLayout_ = {dev, bindings};
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
	vpp::nameHandle(dsLayout_, "LensFlare:dsLayout");
	vpp::nameHandle(pipeLayout_, "LensFlare:pipeLayout");

	vpp::ShaderModule shader(dev, deferred_lens_comp_data);

	ComputeGroupSizeSpec groupSizeSpec(groupDimSize, groupDimSize);
	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout_;
	cpi.stage.module = shader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";
	cpi.stage.pSpecializationInfo = &groupSizeSpec.spec;
	pipe_ = {dev, cpi};
	vpp::nameHandle(pipe_, "LensFlare:pipe");

	ds_ = {data.initDs, pci.wb.alloc.ds, dsLayout_};
	ubo_ = {data.initUbo, pci.wb.alloc.bufHost, sizeof(params),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
	blur_ = blur.createInstance(data.initBlur);
}

void LensFlare::init(InitData& data, const PassCreateInfo&,
		const GaussianBlur& blur) {
	ds_.init(data.initDs);
	ubo_.init(data.initUbo);
	blur.initInstance(blur_, data.initBlur);
	vpp::nameHandle(ds_, "LensFlare:ds");
}

void LensFlare::createBuffers(InitBufferData& data, const tkn::WorkBatcher& wb,
		vk::Extent2D size) {
	auto& dev = wb.dev;
	size.width = std::max(size.width >> 1, 1u);
	size.height = std::max(size.height >> 1, 1u);

	auto usage = vk::ImageUsageBits::storage |
		vk::ImageUsageBits::sampled;
	auto info = vpp::ViewableImageCreateInfo(vk::Format::r16g16b16a16Sfloat,
		vk::ImageAspectBits::color, size, usage);
	dlg_assert(vpp::supported(dev, info.img));

	target_ = {data.initTarget, wb.alloc.memDevice, info.img,
		dev.deviceMemoryTypes()};
	tmpTarget_ = {data.initTmpTarget, wb.alloc.memDevice, info.img,
		dev.deviceMemoryTypes()};
	data.viewInfo = info.view;
}

void LensFlare::initBuffers(InitBufferData& data, vk::ImageView lightInput,
		const GaussianBlur& blur) {
	target_.init(data.initTarget, data.viewInfo);
	tmpTarget_.init(data.initTmpTarget, data.viewInfo);
	vpp::nameHandle(target_, "LensFlare:target");
	vpp::nameHandle(tmpTarget_, "LensFlare:tmpTarget");

	blur.updateInstance(blur_, target_.vkImageView(), tmpTarget_.vkImageView());

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler({{{}, lightInput,
		vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.storage({{{}, target_.vkImageView(),
		vk::ImageLayout::general}});
	dsu.uniform({{{ubo_}}});
}

void LensFlare::record(vk::CommandBuffer cb, const GaussianBlur& blur,
		vk::Extent2D size) {
	vpp::DebugLabel(target_.device(), cb, "LensFlare");

	vk::ImageMemoryBarrier barrier;
	barrier.image = target_.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

	size.width = std::max(size.width >> 1, 1u);
	size.height = std::max(size.height >> 1, 1u);
	auto gx = (size.width + groupDimSize - 1) / groupDimSize;
	auto gy = (size.height + groupDimSize - 1) / groupDimSize;

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {{ds_}});
	vk::cmdDispatch(cb, gx, gy, 1);

	// make result of compute shader visible
	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = {};
	barrier.newLayout = blur.dstScope().layout;
	barrier.dstAccessMask = blur.dstScope().access;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};

	vk::ImageMemoryBarrier tmpBarrier;
	tmpBarrier.image = tmpTarget_.image();
	tmpBarrier.oldLayout = vk::ImageLayout::undefined;
	tmpBarrier.srcAccessMask = {};
	tmpBarrier.newLayout = blur.dstScopeTmp().layout;
	tmpBarrier.dstAccessMask = blur.dstScope().access;
	tmpBarrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};

	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::computeShader |
		vk::PipelineStageBits::topOfPipe,
		blur.dstScope().stages, {}, {}, {}, {{barrier, tmpBarrier}});

	auto kernel = GaussianBlur::createKernel(blurHSize, blurFac);
	blur.record(cb, blur_, size, {target_.image()}, {tmpTarget_.image()},
		kernel);
}

void LensFlare::updateDevice() {
	auto map = ubo_.memoryMap();
	auto span = map.span();
	tkn::write(span, params);
	map.flush();
}

SyncScope LensFlare::dstScopeLight() const {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead
	};
}

SyncScope LensFlare::srcScopeTarget() const {
	// TODO: this should just be the src scope of target
	// as returned by GaussianBlur
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderWrite
	};
}
