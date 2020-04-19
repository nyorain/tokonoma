#pragma once

#include <tkn/types.hpp>
#include <tkn/render.hpp>
#include <vpp/image.hpp>
#include <vpp/handles.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vkpp/enums.hpp>
#include <nytl/vec.hpp>
#include <vector>

using namespace tkn::types;

class TAAPass {
public:
	// must match the format declared in the compute shader
	static constexpr auto format = vk::Format::r16g16b16a16Sfloat;

	static constexpr auto flagPassthrough = (1 << 0u);
	static constexpr auto flagClosestDepth = (1 << 1u);
	static constexpr auto flagTonemap = (1 << 2u);
	static constexpr auto flagDepthReject = (1 << 3u);
	static constexpr auto flagColorClip = (1 << 4u);
	static constexpr auto flagLuminanceWeigh = (1 << 5u);

	struct {
		// Minimum and maximum factors of how much history is used, based
		// on the luminance difference. Independent from the color
		// clipping we do anyways. When both are the same, luminance
		// difference does not matter.
		float minFac {0.95};
		float maxFac {0.995};
		// Weighting is additionally multiplied by
		// exp(-velWeight * (length(vel))), vel in screen space.
		// If you set this to 0, the factor will always be 1.
		float velWeight {1.f};
		u32 flags {flagColorClip | flagClosestDepth};
	} params;

public:
	TAAPass() = default;

	void init(vpp::Device& dev, vk::Sampler linearSampler);
	void initBuffers(vk::Extent2D size, vk::ImageView renderInput,
		vk::ImageView depthInput, vk::ImageView velInput);
	void record(vk::CommandBuffer cb, vk::Extent2D size);
	void updateDevice(float near, float far);
	nytl::Vec2f nextSample();

	auto& targetImage() const { return hist_; }
	auto& targetView() const { return inHist_; }

	// TODO
	// tkn::SyncScope srcScope() const;
	// tkn::SyncScope dstScopeInput() const;

protected:
	vpp::Image hist_; // 2 array layers for in, out
	vpp::ImageView inHist_;
	vpp::ImageView outHist_;

	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;
	vpp::SubBuffer ubo_;

	std::vector<nytl::Vec2f> samples_;
	unsigned sampleID_ {};
};
