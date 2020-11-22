#include <tkn/passes/blur.hpp>
#include <tkn/util.hpp>
#include <vpp/debug.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/shader.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>

#include <shaders/deferred.gblur.comp.h>

// TODO: allow src and tmp images to have different sizes.
// Requires a fix in the shader, we have to query size of src
// and dst individually and don't assume them the same.
// See note in BloomPass (bloom.cpp) for usecase.

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

// Rounds up to the next odd number.
// Might only works for positive numbers.
float roundOdd(float x) {
	return std::floor(x / 2) * 2 + 1;
}

} // anon namespace

namespace tkn {

// GaussianBlur
GaussianBlur::Kernel GaussianBlur::createKernel(unsigned hsize, float fac) {
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
		auto o1 = i - start;
		auto o2 = i - start + 1;
		float o = (o1 * w1 + o2 * w2) / w;
		kernel[++idx] = {o, w};
	}

	dlg_assert(idx < 16);
	return kernel;
}

void GaussianBlur::init(const vpp::Device& dev, vk::Sampler linearSampler) {
	auto bindings = std::array {
		// important to use linear sampler here, our shader is optimized,
		// reads multiple pixels in a single fetch via linear sampling
		vpp::descriptorBinding( // input color
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, &linearSampler),
		vpp::descriptorBinding( // output color, blurred
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute)
	};

	vk::PushConstantRange pcr;
	pcr.size = 128;
	pcr.stageFlags = vk::ShaderStageBits::compute;

	dsLayout_.init(dev, bindings);
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
	dsu1.imageSampler(view);
	dsu1.storage(tmp);

	vpp::DescriptorSetUpdate dsu2(ds.pong);
	dsu2.imageSampler(tmp);
	dsu2.storage(view);
}

void GaussianBlur::record(vk::CommandBuffer cb, const Instance& instance,
		const vk::Extent2D& dstSize, Image srcDst, Image tmp,
		const Kernel& kernel, vk::ImageAspectBits aspect) const {
	dlg_assert(dstSize.width > 0 && dstSize.height > 0);
	dlg_assert(kernel[0].x > 1);
	vpp::DebugLabel debugLabel(instance.ping.device(), cb, "GaussianBlur");

	// basically ceil(dstSize / float(groupDimSize))
	auto gx = ceilDivide(dstSize.width, groupDimSize);
	auto gy = ceilDivide(dstSize.height, groupDimSize);

	// horizontal pass
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe_);
	vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute, 0,
		sizeof(kernel), kernel.data());
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
	vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::compute, 0, 4, &nx);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout_, 0, {instance.pong});
	vk::cmdDispatch(cb, gx, gy, 1);
}

SyncScope GaussianBlur::dstScope() {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead
	};
}

SyncScope GaussianBlur::srcScope() {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderWrite
	};
}

SyncScope GaussianBlur::dstScopeTmp() {
	return {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderWrite
	};
}

SyncScope GaussianBlur::srcScopeTmp() {
	return {
		vk::PipelineStageBits::computeShader,
		// We use undefined here because the contents are undefined
		vk::ImageLayout::undefined,
		vk::AccessBits::shaderWrite
	};
}

} // namespace tkn
