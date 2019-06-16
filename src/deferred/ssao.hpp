#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

using namespace doi::types;

/// SSAO and blur pass.
/// Will use a compute pipeline if possible (r8Unorm storage image supported).
class SSAOPass {
public:
	static constexpr auto sampleCount = 16u;
	static constexpr auto groupDimSize = 8u; // when using compute pipe
	static constexpr auto format = vk::Format::r8Unorm;

	struct InitData {
		vpp::SubBuffer samplesStage;
		vpp::SubBuffer::InitData initSamplesStage;
		vpp::SubBuffer::InitData initSamples;
		std::vector<nytl::Vec4f> samples;
		vpp::TrDs::InitData initDs;
		vpp::TrDs::InitData initBlurHDs;
		vpp::TrDs::InitData initBlurVDs;
		doi::Texture::InitData initNoise;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vpp::ViewableImage::InitData initBlurTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	SSAOPass() = default;
	void create(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);

	void createBuffers(InitBufferData&, const doi::WorkBatcher&, vk::Extent2D);
	void initBuffers(InitBufferData&, vk::ImageView depth,
		vk::ImageView normals, vk::Extent2D size);
	vk::ImageView targetView() const { return target_.imageView(); }

	/// Inputs:
	/// - ldepth: used as shaderReadOnlyOptimal, sampled
	/// - normals: used as shaderReadOnlyOptimal, sampled
	/// Outputs:
	/// - returns ssao target, r8Unorm
	///   compute: general layout, graphics: shaderReadOnlyOptimal
	void record(vk::CommandBuffer, vk::Extent2D, vk::DescriptorSet sceneDs);

	SyncScope dstScopeDepth() const; // shaderReadOnlyOptimal
	SyncScope dstScopeNormals() const; // shaderReadOnlyOptimal
	// usingComopute() ? general layout : shaderReadOnlyOptimal
	SyncScope srcScopeTarget() const;

	const auto& target() const { return target_; }
	bool usingCompute() const { return !rp_; }

protected:
	// only when rendering
	vpp::RenderPass rp_;
	vpp::Framebuffer fb_;

	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::ViewableImage target_;
	doi::Texture noise_;
	vpp::SubBuffer samples_;

	struct {
		vpp::Framebuffer fb; // only when rendering
		vpp::ViewableImage target;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs dsHorz;
		vpp::TrDs dsVert;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} blur_;
};
