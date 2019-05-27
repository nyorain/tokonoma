// WIP

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

public:
	struct {
		u32 flags {flagFXAA};
		u32 tonemap {tonemapReinhard};
		float exposure {1.f};
		float depthFocus {1.f};
		float dofStrength {0.5f};
	} params;

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

public:
	PostProcessPass() = default;
	void create(InitData&, const PassCreateInfo&, vk::Format output);
	void init(InitData&, const PassCreateInfo&);
	vpp::Framebuffer initFramebuffer(vk::ImageView output,
		vk::ImageView light, vk::ImageView ldepth, vk::Extent2D);

	// expects the render pass to be already active.
	// allows caller to draw screen-space stuff after pp is done.
	void record(vk::CommandBuffer);
	void updateDevice();

	SyncScope dstScopeLight() const;
	SyncScope dstScopeDepth() const;
	vk::RenderPass renderPass() const { return rp_; }

protected:
	vpp::RenderPass rp_;
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::Pipeline pipe_;

	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
};
