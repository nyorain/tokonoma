#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

/// Pass that applies ambient occlusion and emission to the light target.
/// Always a compute pipeline, has no targets on its own.
/// Instead, directly works on the light color buffer (in general layout).
/// Inputs: (are all sampled from in shaderReadOnlyOptimal layout)
/// - albedo buffer is needed for ao colors
/// - emission, will be applied to scene (counts as kind of ao).
/// - ldepth target needed for position reconstruction which
///   in turn is needed to determine the view direction for specular ibl.
/// - normal buffer is needed for ibl. But roughness and metallic information
///   are needed for ibl as well (expected stored in normal buffer).
/// - [optional] ssao input, will be used to weigh ao
class AOPass {
public:
	static constexpr auto groupDimSize = 8u;
	static constexpr u32 flagDiffuseIBL = (1 << 0);
	static constexpr u32 flagSpecularIBL = (1 << 1);
	static constexpr u32 flagEmission = (1 << 2);
	static constexpr u32 flagSSAO = (1 << 3);

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

public:
	struct {
		u32 flags {flagDiffuseIBL | flagSpecularIBL | flagEmission};
		float factor {0.25f};
		float ssaoPow {3.f};
	} params;

public:
	AOPass() = default;
	void create(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);

	void updateInputs(vk::ImageView light,
		vk::ImageView albedo, vk::ImageView emission, vk::ImageView ldepth,
		vk::ImageView normal, vk::ImageView ssao,
		vk::ImageView irradiance, vk::ImageView filteredEnv,
		unsigned filteredEnvLods, vk::ImageView brdflut);

	void record(vk::CommandBuffer, vk::DescriptorSet sceneDs, vk::Extent2D);
	void updateDevice();

	SyncScope scopeLight() const;
	SyncScope dstScopeSSAO() const;
	SyncScope dstScopeGBuf() const; // emission, albedo, ldepth, normals

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	u32 envFilterLods_ {}; // push constant
	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
};

// TODO: does this make sense?
		/// Will automatically set/unset flagSSAO in params.flag depending
		/// on whether this is valid or not.
