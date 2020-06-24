// Advanced luminance pass that uses compute reduction when
// possible for avg luminance calculation.
// Will improve correctness of result since automatic mipmap generation
// does discard a large (!) bunch of pixels for non-power-of-two textures
// (which the render targets usually are). Might also improve performance.
// When r8Unorm storage images are not supported we still fall back
// to automatic mipmap generation.
// TODO(bug): even when compute shaders aren't available we can make the
// luminance calculation using mipmaps correct by simply using
// a power-of-two luminance target, clearing it with black before
// rendering (via renderpass, then just use a smaller viewport+scissor) and
// applying the final factor onto the result like we do with compute
// shaders.

#pragma once

#include <tkn/render.hpp>
#include <tkn/defer.hpp>
#include <tkn/types.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/image.hpp>

namespace tkn {

/// Gets the average luminance value from the luminance buffer.
/// If possible, will use a compute shader. Otherwise will use mipmap
/// generation via linear blitting, that has a lower quality though.
class LuminancePass {
public:
	// log2(luminance) stored before tonemapping
	// TODO(perf, low): we could fit that in r8Unorm with a
	// transformation function. we don't need that high precision
	// (higher precision near luminance 0 though)
	// make sure to first invert that transformation function then
	// before each combine.
	static constexpr auto format = vk::Format::r16Sfloat;
	static constexpr auto extractGroupDimSize = 8u;

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

	// Need to recreate pass when changing these.
	bool compute = true;
	unsigned mipGroupDimSize = 8u;

public:
	LuminancePass() = default;
	void create(InitData&, WorkBatcher& wb, vk::Sampler nearest,
		vk::ShaderModule fullscreenVertMod);
	void init(InitData&);

	void createBuffers(InitBufferData&, tkn::WorkBatcher&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::ImageView light, vk::Extent2D);

	/// Expects log(luminance) to already be stored in the mip level 0
	/// of the luminance target. The target must have a full mipmap chain.
	void record(vk::CommandBuffer cb, vk::Extent2D extent);
	float updateDevice(); // returns the luminance of the last frame

	bool usingCompute() const { return !extract_.rp; }
	const auto& target() const { return target_; }

	SyncScope dstScopeLight() const; // layout: shaderReadOnlyOptimal
	// SyncScope for the first mip level of the luminance target.
	// Depending on whether a compute shader is used or not, will
	// be in transferSrcOptimal or shaderReadOnlyOptimal layout.
	SyncScope srcScopeTarget() const;

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
		float factor {};
	} mip_;
};

} // namespace tkn
