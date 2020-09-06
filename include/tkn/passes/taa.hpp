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

namespace tkn {

// Temporal anti-aliasing pass. Implements different rejection
// mechanisms but always reprojects using a velocity buffer.
class TAAPass {
public:
	// Format used for the temporal accumulation buffer.
	// Must match the format declared in the compute shader
	static constexpr auto format = vk::Format::r16g16b16a16Sfloat;

	// If this is set, will not do any TAA but basically just copy
	// the input to the temporal buffer.
	static constexpr auto flagPassthrough = (1 << 0u);
	// Whether or not reprojection should be done for the closest texel
	// in a 3x3 neighborhood. This is needed to achieve AA under
	// movement.
	static constexpr auto flagClosestDepth = (1 << 1u);
	// Whether TAA weighing and rejection should happen in a tonemapped
	// space. This doesn't change that the pass expects linear input
	// and also outputs in linear space.
	static constexpr auto flagTonemap = (1 << 2u);
	// Whether history samples should be rejected based on inverse reprojected
	// depth in a neighborhood. This rejection mechanism never rejects
	// incorrectly but doesn't catch all cases and will therefore leave
	// some ghosting. But when it works it should give high quality AA
	// and not cause much blur.
	static constexpr auto flagDepthReject = (1 << 3u);
	// Whether history samples should be color-clipped. This brute-force
	// approach pretty much always works but can diminish AA quality
	// severly. It furthermore might lead to ghost blur as it always clips
	// based on a neighboorhood.
	static constexpr auto flagColorClip = (1 << 4u);
	// Whether the weighing between current and history sample should be
	// modified by the luminance difference.
	static constexpr auto flagLuminanceWeigh = (1 << 5u);
	// Whether the history is sampled with a bicubic filter.
	static constexpr auto flagBicubic = (1 << 6u);

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
		// Increasing this will lead to movement instability but less blur.
		float velWeight {0.25f};
		u32 flags {flagColorClip | flagClosestDepth};
	} params;

	// When this is set to true, will ignore history contents in
	// the next frame, but also unset the flag then.
	// This is needed e.g. after buffer recreation (when the history
	// contents are invalid) but can be useful for debuggin as well,
	// to see how the samples converge.
	bool resetHistory {true}; // ignore history contents for one frame

public:
	TAAPass() = default;

	// - linearSampler: a sampler with linear sampling. Addressing
	//   modes, anisotropy and such don't matter.
	// - nearestSampler: sampler with nearest sampling.
	void init(vpp::Device& dev, vk::Sampler linearSampler,
		vk::Sampler nearestSampler);
	// - renderInput: view to the rgba image that will hold the rendered
	//   scene content when this pass is run.
	// - depthInput: view to the depth image that will hold the depth
	//   of the rendered scene when this pass is run.
	// - velInput: view to the (at least RGB) image that holds the velocity
	//   of the scene in screen space when this pass is run. The velocity
	//   is given in ndc space.
	void initBuffers(vk::Extent2D size, vk::ImageView renderInput,
		vk::ImageView depthInput, vk::ImageView velInput);
	// Records this pass into the given command buffer.
	// The given size must be the same that initBuffers was last called with.
	void record(vk::CommandBuffer, vk::Extent2D size);
	// Updates the device parameters. The 'near' and 'far' parameters
	// of the used perspective projection are needed to reconstruct the linear
	// depth from the depth texture, as needed for depth-based rejection.
	// Needs to be called every frame.
	void updateDevice(float near, float far);
	// Returns the next jittering sample, in ndc space.
	// Should be called once per frame and the projection matrix updated
	// with that jittering offset.
	nytl::Vec2f nextSample();

	// SyncScope for the created target view.
	// It will hold the antialiased image after this pass is run.
	tkn::SyncScope srcScope() const;
	// SyncScope for all inputs: rendered color, depth and velocity.
	tkn::SyncScope dstScopeInput() const;

	auto& targetImage() const { return hist_; }
	auto& targetView() const { return inHist_; }

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

} // namesapce tkn
