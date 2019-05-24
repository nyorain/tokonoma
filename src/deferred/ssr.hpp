#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>
#include <dlg/dlg.hpp>

using namespace doi::types;

// NOTE: the only reason we are using rgba16f encoding here is that
// 8 bits for x,y just aren't enough: only 255 pixels can be represented
// but we want to correctly show reflections for *all* pixels on the screen
// (and not just in blocks). We don't normalize uv for better
// precision. We could use rgba16fUnorm but that isn't guaranteed
// to be supported as storage image format.

/// Constructs screen space reflection information from depth and normals.
/// For a given pixel p the output holds following information:
/// - x,y: the pixel coordinates on the light buffer where the reflection
///   ray ends. Not normalized, e.g. x in range [0, width].
///   Undefined if it the ray doesn't hit a reflection for p.
/// - z: how much the reflection is blurred. Depens on roughness
///   of the surface and length of the reflection ray.
///   Undefined if the ray doesn't hit a reflection for p.
/// - w: reflection factor (how strong is it). Depends on roughness,
///   viewing angle and some artefact-preventing/smoothing conditions.
///   0 if there is no reflection for p.
class SSRPass {
public:
	static constexpr auto format = vk::Format::r16g16b16a16Sfloat;
	static constexpr unsigned groupDimSize = 8u;

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	// See ssr.comp for more details.
	// most parameters should be based on scene size
	// getting these right is extremely important, ssr succeeds or fails
	// depending on these!
	struct {
		u32 marchSteps = 32u;
		u32 binarySearchSteps = 4u;
		float startStepSize = 0.02;
		float stepFactor = 1.1;
		float ldepthThreshold = 0.01;
		float roughnessFacPow = 1.f;
	} params;

public:
	SSRPass() = default;
	void create(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);

	void createBuffers(InitBufferData&, const doi::WorkBatcher&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::ImageView ldepth,
		vk::ImageView normals);

	/// Inputs:
	/// - depth: used as shaderReadOnlyOptimal; sampled
	/// - normals: used as shaderReadOnlyOptimal; sampled
	/// Output:
	/// - ssr (returned): general layout, 1 mip level
	RenderTarget record(vk::CommandBuffer, RenderTarget& depth,
		RenderTarget& normals, vk::DescriptorSet sceneDs, vk::Extent2D);
	void updateDevice();

	vk::ImageView targetView() const { return target_.imageView(); }

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::ViewableImage target_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;

	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
};
