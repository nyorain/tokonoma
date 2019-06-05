#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

// TODO: we can probably apply bloom and scattering in post processing
// pass, right? they don't really need fxaa/dof (dof for bloom is
// reasonable but not really needed i guess; dof for scattering
// is just plain wrong!).
// This should probably be renamed into SSRApply or sth (or should be
// part of ssr pass to begin with). This is quite expensive due to
// the additional buffer and therefore makes bloom and scattering
// more expensive than they have to be.
// Rework with proper ssr blurring!

/// Postprocessing pass that applies light scattering, ssr, DOF and bloom.
/// Always a compute pipeline. Outputs its contents into a given buffer,
/// re-used from an earlier stage (with rgba16f format).
class CombinePass {
public:
	static constexpr unsigned groupDimSize = 8u;
	static constexpr u32 flagScattering = (1 << 0);
	static constexpr u32 flagSSR = (1 << 1);
	static constexpr u32 flagBloom = (1 << 2);
	static constexpr u32 flagBloomDecrease = (1 << 3);

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

public:
	struct {
		u32 flags {flagBloomDecrease};
		float bloomStrength {0.15f};
		float scatterStrength {0.2f};
	} params;

public:
	CombinePass() = default;
	void create(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);
	void updateInputs(vk::ImageView output, vk::ImageView light,
		vk::ImageView ldepth, vk::ImageView bloom,
		vk::ImageView ssr, vk::ImageView scattering);
	void record(vk::CommandBuffer, vk::Extent2D);
	void updateDevice();

	SyncScope dstScopeBloom() const;
	SyncScope dstScopeSSR() const;
	SyncScope dstScopeLight() const;
	SyncScope dstScopeDepth() const;
	SyncScope dstScopeScatter() const;
	SyncScope scopeTarget() const;

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;
	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
};
