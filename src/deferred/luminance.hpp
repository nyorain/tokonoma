// WIP: advanced luminance pass that uses compute reduction when
// possible for avg luminance calculation.
// Will improve correctness of result since automatic mipmap generation
// does discard a large (!) bunch of pixels for non-power-of-two textures
// (which the render targets usually are). Might also improve performance.
// When r8Unorm storage images are not supported we still fall back
// to automatic mipmap generation.

#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

/// Gets the average luminance value from the luminance buffer.
/// If possible, will use a compute shader. Otherwise will use mipmap
/// generation via linear blitting, that has a lower quality though.
class LuminancePass {
public:
	// log2(luminance) stored before tonemapping
	// TODO: we could fit that in r8Unorm with a
	// transformation function. we don't need that high precision
	// (higher precision near luminance 0 though)
	// make sure to first invert that transformation function then
	// before each combine.
	static constexpr auto format = vk::Format::r16Sfloat;
	static constexpr auto extractGroupDimSize = 8u;
	static constexpr auto mipGroupDimSize = 4u;
	static_assert((mipGroupDimSize & (mipGroupDimSize - 1)) == 0,
		"mipGroupDimSize must be a power of 2");

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initDstBuffer;
		std::vector<vpp::TrDs::InitData> initLevels;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
	};

	// Luninance vector. dot(light.rgb, luminance) will be used to
	// calculate the luminance. Must rerecord when changed.
	// See https://stackoverflow.com/questions/596216 for a discussion
	// about different conventions.
	std::array<float, 3> luminance {0.25, 0.65, 0.1};

public:
	LuminancePass() = default;
	void create(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);
	void createBuffers(InitBufferData&, const doi::WorkBatcher&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::ImageView light, vk::Extent2D);

	/// Expects log(luminance) to already be stored in the mip level 0
	/// of the luminance target. The target must have a full mipmap chain.
	void record(vk::CommandBuffer cb, RenderTarget& light,
		vk::Extent2D extent);
	float updateDevice(); // returns the luminance of the last frame

	bool usingCompute() const { return !extract_.rp; }

protected:
	vpp::ViewableImage target_;
	vpp::SubBuffer dstBuffer_;
	vpp::MemoryMapView dstBufferMap_;

	// First part of this pass: extract the luminance from the light
	// target. If possible, will use a compute pipeline, otherwise
	// a graphics pipeline (that's what rp, fb are for).
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::RenderPass rp; // graphics pipe only
		vpp::Framebuffer fb; // graphics pipe only
	} extract_;

	// Second part of this pass: calculate the geometric mean of the
	// luminance image.
	struct MipLevel {
		vpp::ImageView view;
		vpp::TrDs ds;
		unsigned target {}; // next/target level
	};

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		std::vector<MipLevel> levels;
		unsigned target0;
		vpp::Sampler sampler; // linear, black border, clamp to border
	} mip_;
};
