#pragma once

#include "pass.hpp"

#include <tkn/render.hpp>
#include <tkn/types.hpp>

using namespace tkn::types;

// Optimized two-pass general purpose gaussian blur with variable kernel size.
// Image (src/dst and temporary ping-pong image) have to be supplied by
// the user. Supports kernel sizes up to 61x61 (limited by the guaranteed
// push constant size).
class GaussianBlur {
public:
	static constexpr auto groupDimSize = 16;

	using Kernel = std::array<Vec2f, 16>; // 128 bytes

	// - hsize: the side of one kernel size.
	//   E.g. if you want an NxN kernel (N odd) you have to pass
	//   (N - 1) / 2. For a 9x9 kernel, hsize = 4. Will always round
	//   up to the next even hsize, i.e. hsize = 5 and hsize = 6 will
	//   both result in a 13x13 kernel. Must be smaller than 31.
	// - fac: factor for how far down in the pascals values
	//   should be used to generate the kernel. Can be seen
	//   as filter sigma normalized to hsize.
	//   Must be greater or equal to 1. Larger factors mean stronger blur
	//   but potentially less quality (cutting of the edges of the curve),
	//   getting closer to a box filter.
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

	SyncScope srcScope() const;
	SyncScope dstScope() const;
	SyncScope srcScopeTmp() const;
	SyncScope dstScopeTmp() const;

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

// Pass that performs a downscale and bias/scaling.
class HighLightPass {
public:
	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	static constexpr auto groupDimSize = 16;

	/// How many levels the created image should have.
	/// In any case, will only fill the first level.
	/// Changing this requires buffer recreation.
	unsigned numLevels = 1u;

	struct {
		float bias = -0.4;
		float scale = 1.5;
	} params;

public:
	HighLightPass() = default;
	void create(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);

	void createBuffers(InitBufferData& data, const tkn::WorkBatcher&,
		vk::Extent2D size);
	void initBuffers(InitBufferData& data, vk::ImageView lightInput,
		vk::ImageView emissionInput);

	void record(vk::CommandBuffer cb, vk::Extent2D);
	void updateDevice();

	SyncScope dstScopeEmission() const;
	SyncScope dstScopeLight() const;
	SyncScope srcScopeTarget() const; // only for the first level

	// The target will have 1/2 the size of the main buffers
	const auto& target() const { return target_; }

protected:
	vpp::SubBuffer ubo_;
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;

	vpp::ViewableImage target_;
};

/// Given an image of light highlights, will produce an image
/// of lens flare artefacts.
class LensFlare {
public:
	unsigned blurHSize = 24;
	float blurFac = 1.5;

	struct {
		u32 numGhosts {5};
		float ghostDispersal {0.4};
		float distortion {10.0};
		float haloWidth {0.5};
	} params;

public:
	static constexpr auto groupDimSize = 16u;

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
		GaussianBlur::InstanceInitData initBlur;
	};

	struct InitBufferData {
		vpp::Image::InitData initTarget;
		vpp::Image::InitData initTmpTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	LensFlare() = default;
	void create(InitData&, const PassCreateInfo&, const GaussianBlur& blur);
	void init(InitData&, const PassCreateInfo&, const GaussianBlur& blur);

	void createBuffers(InitBufferData&, const tkn::WorkBatcher&,
		vk::Extent2D, const GaussianBlur&);
	void initBuffers(InitBufferData&, vk::ImageView lightInput,
		const GaussianBlur&);

	void record(vk::CommandBuffer cb, const GaussianBlur&, vk::Extent2D);
	void updateDevice();

	SyncScope dstScopeLight() const;
	SyncScope srcScopeTarget() const;

	const auto& target() const { return target_; }

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;
	vpp::ViewableImage target_;

	// needed for first blur pass
	// TODO(perf): might be able to share this with others passes
	vpp::ViewableImage tmpTarget_;
	GaussianBlur::Instance blur_;
};
