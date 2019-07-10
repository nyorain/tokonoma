#pragma once

#include "pass.hpp"
#include <tkn/render.hpp>

// TODO: implement alternative that just uses an additional
// output in light pass
// TODO: could optionally make this a compute pass (fullscreen)
// TODO: for point lights, we don't want fullscreen, instead use
//   the light volume boxes from light pass (geomLight)

/// Simple pass that calculates light scattering for one light onto an
/// r8Unorm fullscreen target.
/// Needs depth (and based on scattering algorithm also the lights
/// shadow map) as input
class LightScatterPass {
public:
	static constexpr vk::Format format = vk::Format::r8Unorm;
	static constexpr u32 flagShadow = 1 << 0u;
	static constexpr u32 flagAttenuation = 1 << 1u; // only point lights

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	// See scatter.glsl, point/dirScatter.frag
	struct {
		u32 flags {flagShadow | flagAttenuation};
		float fac {1.f};
		float mie {0.1f};
	} params;

	// Needs recreation after being changed, specialization constant
	// to allow driver unrolling at pipeline compliation time
	u32 sampleCount = 10u;

public:
	LightScatterPass() = default;
	void create(InitData&, const PassCreateInfo&, bool directional);
	void init(InitData&, const PassCreateInfo&);

	void createBuffers(InitBufferData&, const tkn::WorkBatcher&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::Extent2D,
		vk::ImageView depth);

	void record(vk::CommandBuffer, vk::Extent2D,
		vk::DescriptorSet scene, vk::DescriptorSet light);
	void updateDevice();
	const auto& target() const { return target_; }

	SyncScope srcScopeTarget() const;
	SyncScope dstScopeDepth() const;

protected:
	vpp::RenderPass rp_;
	vpp::Framebuffer fb_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	// vpp::Pipeline pointPipe_;
	// vpp::Pipeline dirPipe_;
	vpp::ViewableImage target_;

	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
};
