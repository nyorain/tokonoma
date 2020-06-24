#pragma once

#include <tkn/defer.hpp>
#include <tkn/bits.hpp>
#include <tkn/texture.hpp>
#include <tkn/types.hpp>
#include <tkn/render.hpp>
#include <tkn/render.hpp>
#include <tkn/types.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/handles.hpp>
#include <vpp/image.hpp>

#include <array>

namespace tkn {

// Optimized two-pass general purpose gaussian blur with variable kernel size.
// Image (src/dst and temporary ping-pong image) have to be supplied by
// the user. Supports kernel sizes up to 61x61 (limited by the guaranteed
// push constant size).
class GaussianBlur {
public:
	static constexpr auto groupDimSize = 8; // 2D; 64 WorkGroup invocations

	// Representation of the blur kernel used on the GPU.
	// Encodes the offsets and weights needed for linear sampling
	// as described here:
	// http://rastergrid.com/blog/2010/09/efficient-gaussian-blur-with-linear-sampling/
	// x component is the offset
	// y component is the weight
	// The record function will encode additional information (kernel size,
	// horizontal or vertical blur) in the x component of the first value,
	// since the first offset is always 0.0 anyways.
	using Kernel = std::array<Vec2f, 16>; // 128 bytes

	// - hsize: the side of one kernel size.
	//   E.g. if you want an NxN kernel (N odd) you have to pass
	//   (N - 1) / 2. For a 9x9 kernel, hsize = 4. Will always round
	//   up to the next even hsize, i.e. hsize = 5 and hsize = 6 will
	//   both result in a 13x13 kernel. Must be smaller than 31.
	// - fac: factor for how far down in pascals triangle values
	//   should be used to generate the kernel. Can be seen
	//   as filter sigma normalized to hsize.
	//   Must be greater or equal to 1. Larger factors mean stronger blur
	//   but potentially less quality (cutting of the edges of the curve),
	//   getting closer to a box filter. 1.0 Means a perfect
	//   gaussian blur.
	// The operation is not particularly expensive and can be performed
	// every recording.
	static Kernel createKernel(unsigned hsize, float fac = 1.5);

	struct InstanceInitData {
		vpp::TrDs::InitData ping;
		vpp::TrDs::InitData pong;
	};

	struct Instance {
		vpp::TrDs ping;
		vpp::TrDs pong;
	};

	struct Image {
		vk::Image image;
		u32 mipLevel {};
		u32 arrayLayer {};
	};

public:
	GaussianBlur() = default;
	void init(const vpp::Device&, vk::Sampler linearSampler);

	// Creates an instance of using this pass. Must be updated via
	// updateInstance (can be used multiple times) before using
	// it to record the commands.
	Instance createInstance(InstanceInitData& data,
		vpp::DescriptorAllocator* alloc = nullptr) const;
	void initInstance(Instance& ini, InstanceInitData& data) const;

	// The given image views:
	// - must have exactly one layer and level
	// - must have the sampled image and storage image usage
	// - must have rgba16f format
	// - must have the same size.
	// Must not be called while the instance is in use, will perform
	// a descriptor set update. Recording this pass wil expect the
	// contents to be initially in view and will write it to view in the
	// end (but needs tmp in between). Calling this requires a rerecord.
	void updateInstance(Instance&, vk::ImageView srcDst,
		vk::ImageView tmp) const;

	// The given extent must be the size of the ImageView used for the
	// given instance.
	// The images (with all meta information like aspect, mip and layer)
	// must match the image view the instance was last updated with.
	// The kernel can be obtained from createKernel.
	void record(vk::CommandBuffer cb, const Instance& instance,
		const vk::Extent2D&, Image srcDst, Image tmp, const Kernel& kernel,
		vk::ImageAspectBits aspects = vk::ImageAspectBits::color) const;

	static SyncScope srcScope(); // for target
	static SyncScope dstScope(); // for target
	static SyncScope srcScopeTmp();
	static SyncScope dstScopeTmp();

	const vpp::Device& device() const { return pipe_.device(); }

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

} // namespace blur
