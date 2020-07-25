#pragma once

// TODO: abolish in favor passes moved to tkn/passes

#include "pass.hpp"

#include <tkn/render.hpp>
#include <tkn/types.hpp>
#include <tkn/passes/blur.hpp>

using namespace tkn::types;

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

	void createBuffers(InitBufferData& data, tkn::WorkBatcher&,
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
		tkn::GaussianBlur::InstanceInitData initBlur;
	};

	struct InitBufferData {
		vpp::Image::InitData initTarget;
		vpp::Image::InitData initTmpTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	LensFlare() = default;
	void create(InitData&, const PassCreateInfo&, const tkn::GaussianBlur& blur);
	void init(InitData&, const PassCreateInfo&, const tkn::GaussianBlur& blur);

	void createBuffers(InitBufferData&, tkn::WorkBatcher&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::ImageView lightInput,
		const tkn::GaussianBlur&);

	void record(vk::CommandBuffer cb, const tkn::GaussianBlur&, vk::Extent2D);
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
	tkn::GaussianBlur::Instance blur_;
};
