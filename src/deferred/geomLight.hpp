#pragma once

#include "pass.hpp"
#include "timeWidget.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

#include <stage/scene/scene.hpp> // only fwd
#include <stage/scene/light.hpp> // only fwd

// fwd
namespace doi {

class PointLight;
class DirLightj;
class Scene;
class Environment;

} // namespace doi

// TODO: optionally re-implement ldepth mip levels, helpful for ssao
// only add support for creating it with n levels here

/// Render geometry and lights, deferred.
class GeomLightPass {
public:
	static constexpr auto normalsFormat = vk::Format::r16g16b16a16Snorm;
	static constexpr auto ldepthFormat = vk::Format::r16Sfloat;
	static constexpr auto albedoFormat = vk::Format::r8g8b8a8Srgb;
	static constexpr auto emissionFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto linearDepthFormat = vk::Format::r16Sfloat;
	static constexpr auto lightFormat = vk::Format::r16g16b16a16Sfloat;

	struct InitData {
		vpp::TrDs::InitData initLightDs;
		vpp::TrDs::InitData initAoDs;
		vpp::SubBuffer::InitData initAoUbo;
	};

	struct InitTarget {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

	struct InitBufferData {
		InitTarget initNormals;
		InitTarget initAlbedo;
		InitTarget initDepth;
		InitTarget initEmission;
		InitTarget initLight;
	};

	// TODO: duplication ao.hpp
	static constexpr u32 flagDiffuseIBL = (1 << 0);
	static constexpr u32 flagSpecularIBL = (1 << 1);
	static constexpr u32 flagEmission = (1 << 2);
	struct {
		u32 flags {flagDiffuseIBL | flagSpecularIBL | flagEmission};
		float factor {0.08f};
	} aoParams;

public:
	GeomLightPass() = default;
	void create(InitData&, const PassCreateInfo&,
		SyncScope dstNormals,
		SyncScope dstAlbedo,
		SyncScope dstEmission,
		SyncScope dstDepth,
		SyncScope dstLDepth,
		SyncScope dstLight,
		bool ao);
	void init(InitData&);

	void createBuffers(InitBufferData&, const doi::WorkBatcher&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::Extent2D, vk::ImageView depth,
		// following parameters only required when pass includes ao
		vk::ImageView irradiance, vk::ImageView filteredEnv,
		unsigned filteredEnvLods, vk::ImageView brdflut);

	// If an environment is given, will render the skybox
	void record(vk::CommandBuffer cb, const vk::Extent2D&,
		vk::DescriptorSet sceneDs, const doi::Scene& scene,
		nytl::Span<doi::PointLight>, nytl::Span<doi::DirLight>,
		vpp::BufferSpan boxIndices, const doi::Environment* env,
		TimeWidget& time);
	void updateDevice();
	// SyncScope srcScopeLight() const;

	const auto& normalsTarget() const { return normals_; }
	const auto& ldepthTarget() const { return ldepth_; }
	const auto& emissionTarget() const { return emission_; }
	const auto& albedoTarget() const { return albedo_; }
	const auto& lightTarget() const { return light_; }

	vk::RenderPass renderPass() const { return rp_; }
	vk::Framebuffer framebuffer() const { return fb_; }
	vk::PipelineLayout geomPipeLayout() const { return geomPipeLayout_; }
	vk::PipelineLayout lightPipeLayout() const { return lightPipeLayout_; }
	vk::Pipeline geomPipe() const { return geomPipe_; }
	vk::Pipeline dirLightPipe() const { return dirLightPipe_; }
	vk::Pipeline pointLightPipe() const { return pointLightPipe_; }
	bool renderAO() const { return aoPipe_; }

protected:
	vpp::RenderPass rp_;
	vpp::Framebuffer fb_;

	vpp::ViewableImage normals_;
	vpp::ViewableImage albedo_;
	vpp::ViewableImage emission_;
	vpp::ViewableImage ldepth_; // color, for linear sampling/mipmaps
	vpp::ViewableImage light_;

	vpp::PipelineLayout geomPipeLayout_;
	vpp::Pipeline geomPipe_;
	vpp::Pipeline blendPipe_;

	vpp::TrDsLayout lightDsLayout_; // input attachment bindings
	vpp::TrDs lightDs_; // input attachment bindings
	vpp::PipelineLayout lightPipeLayout_;
	vpp::Pipeline dirLightPipe_;
	vpp::Pipeline pointLightPipe_;

	// ao
	vpp::TrDsLayout aoDsLayout_;
	vpp::TrDs aoDs_;
	vpp::PipelineLayout aoPipeLayout_;
	vpp::Pipeline aoPipe_;
	vpp::SubBuffer aoUbo_;
	vpp::MemoryMapView aoUboMap_;
	unsigned aoEnvLods_ {};
};
