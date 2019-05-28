#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

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
		float bloomStrength {0.25f};
		float scatterStrength {0.25f};
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
	SyncScope scopeTarget() const;

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;
	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
};
