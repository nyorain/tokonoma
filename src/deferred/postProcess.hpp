#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

class PostProcessPass {
public:
	static constexpr u32 tonemapClamp = 0u;
	static constexpr u32 tonemapReinhard = 1u;
	static constexpr u32 tonemapUncharted2 = 2u;
	static constexpr u32 tonemapACES = 3u;
	static constexpr u32 tonemapHeijlRichard = 4u;

	static constexpr u32 flagFXAA = (1 << 0u);
	static constexpr u32 flagDOF = (1 << 1u);

	static constexpr u32 modeDefault = 0u; // render normally
	static constexpr u32 modeAlbedo = 1u;
	static constexpr u32 modeNormals = 2u;
	static constexpr u32 modeRoughness = 3u;
	static constexpr u32 modeMetalness = 4u;
	static constexpr u32 modeAO = 5u;
	static constexpr u32 modeAlbedoAO = 6u;
	static constexpr u32 modeSSR = 7u;
	static constexpr u32 modeDepth = 8u;
	static constexpr u32 modeEmission = 9u;
	static constexpr u32 modeBloom = 10u;
	static constexpr u32 modeLuminance = 11u;
	static constexpr u32 modeShadow = 12u;

public:
	struct {
		u32 flags {flagFXAA};
		u32 tonemap {tonemapReinhard};
		float exposure {1.f};
		float depthFocus {1.f};
		float dofStrength {0.05f};
	} params;

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::TrDs::InitData initDebugDs;
		vpp::SubBuffer::InitData initUbo;
	};

public:
	PostProcessPass() = default;
	void create(InitData&, const PassCreateInfo&, vk::Format output);
	void init(InitData&, const PassCreateInfo&);

	void updateInputs(vk::ImageView light, vk::ImageView depth,
		// image views below are only needed in the varioius debug
		// buffer visualization modes
		vk::ImageView normals,
		vk::ImageView albedo,
		vk::ImageView ssao,
		vk::ImageView ssr,
		vk::ImageView bloom,
		vk::ImageView luminance,
		vk::ImageView scatter,
		vk::ImageView shadow);
	vpp::Framebuffer initFramebuffer(vk::ImageView output, vk::Extent2D);

	// expects the render pass to be already active.
	// allows caller to draw screen-space stuff after pp is done.
	void record(vk::CommandBuffer, u32 debugMode);
	void updateDevice();

	SyncScope dstScopeInput() const;
	vk::RenderPass renderPass() const { return rp_; }

protected:
	vpp::RenderPass rp_;
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::Pipeline pipe_;

	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} debug_;
};
