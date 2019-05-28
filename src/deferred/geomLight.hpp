#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

#include <stage/scene/scene.hpp> // only fwd
#include <stage/scene/light.hpp> // only fwd

class AOPass;
class PostProcessPass;

// TODO: re-implement ldepth mip levels

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

public:
	GeomLightPass() = default;
	void create(InitData&, const PassCreateInfo&,
		SyncScope dstNormals,
		SyncScope dstAlbedo,
		SyncScope dstEmission,
		SyncScope dstDepth,
		SyncScope dstLDepth,
		SyncScope dstLight,
		AOPass* ao = nullptr, PostProcessPass* pp = nullptr);
	void init(InitData&);

	void createBuffers(InitBufferData&, const doi::WorkBatcher&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::Extent2D, vk::ImageView depth);

	/// When initialized with ao and pp subpasses,
	// vpp::Framebuffer initFramebuffer(vk::Extent2D, vk::ImageView output);

	void record(vk::CommandBuffer cb, const vk::Extent2D&,
		vk::DescriptorSet sceneDs, const doi::Scene& scene,
		nytl::Span<doi::PointLight>, nytl::Span<doi::DirLight>,
		vpp::BufferSpan boxIndices);
	SyncScope srcScopeLight() const;

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

	vpp::TrDsLayout lightDsLayout_; // input attachment bindings
	vpp::TrDs lightDs_; // input attachment bindings
	vpp::PipelineLayout lightPipeLayout_;
	vpp::Pipeline dirLightPipe_;
	vpp::Pipeline pointLightPipe_;

	struct {
		AOPass* ao {};
		PostProcessPass* pp {};
	} sub_;
};
