// WIP: first concept of a pass abstraction
#pragma once

#include <stage/defer.hpp>
#include <stage/texture.hpp>

#include <vpp/vk.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/handles.hpp>
#include <vpp/image.hpp>

struct SyncScope {
	vk::AccessFlags access;
	vk::PipelineStageFlags stages;
	vk::ImageLayout layout;
};

class SSAOPass {
public:
	struct InitData {
		vpp::SubBuffer samplesStage;
		vpp::SubBuffer noiseStage;
		vpp::TrDs::InitData initDs;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

	static constexpr auto format = vk::Format::r8Unorm;

public:
	void init(InitData&, const doi::WorkBatcher&);
	InitBufferData createBuffers(const vk::Extent2D&);
	void initBuffers(InitBufferData&);

	/// Inputs:
	/// - depth: used as shaderReadOnlyOptimal, sampled
	/// - normals: used as shaderReadOnlyOptimal, sampled
	void record(vk::CommandBuffer cb,
		SyncScope& depthScope, SyncScope& normalScope);
	void updateInputs(vk::ImageView depth, vk::ImageView normals);

	/// Outputs:
	/// - ssao target as shaderReadOnlyOptimal, r8Unorm
	const auto& target() const { return target_; }
	SyncScope targetScope();

	vk::Framebuffer fb() const { return fb_; }
	vk::RenderPass rp() const { return pass_; }

protected:
	vpp::RenderPass pass_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::ViewableImage target_;
	doi::Texture noise_;
	vpp::Framebuffer fb_;
	vpp::SubBuffer samples_;
};

/// Blurs the ssao buffer, has no output on its own.
class SSAOBlurPass {
public:
	void init(const vpp::Device& device, vk::RenderPass ssaorp,
		vk::Sampler linearSampler, vk::Sampler nearestSampler);

	void record(vk::CommandBuffer cb,
		SyncScope& depthScope, SyncScope& ssaoScope);
	void updateInputs(vk::ImageView depth, vk::ImageView ssao,
		vk::Framebuffer ssaofb);
	void initBuffers(const vk::Extent2D&);

protected:
	vk::RenderPass ssaorp_; // from ssao
	vk::Framebuffer ssaofb_; // from ssao

	vpp::Framebuffer fb_;
	vpp::ViewableImage target_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs dsHorz_;
	vpp::TrDs dsVert_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

class LightPass {
public:
	void init(const vpp::Device& device, vk::Sampler linearSampler,
		vk::Sampler nearestSampler);

	void record(vk::CommandBuffer cb,
		vpp::BufferSpan boxIndices,
		SyncScope& depthScope, SyncScope& normalScope,
		SyncScope& albedoScope, SyncScope& emissionScope);
	void updateInputs(vk::ImageView depth, vk::ImageView normal,
		vk::ImageView albedo, vk::ImageView emissionScope);
	void initBuffers(const vk::Extent2D&);

	const auto& target() const { return target_; }
	SyncScope targetScope();

protected:
	vpp::RenderPass pass_;
	vpp::Framebuffer fb_;
	vpp::TrDsLayout dsLayout_; // input
	vpp::TrDs ds_;
	vpp::ViewableImage target_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pointPipe_;
	vpp::Pipeline dirPipe_;
};

/// Blurs the bloom target in place, i.e. has no output.
class BloomBlurPass {
public:
	void init(const vpp::Device& device, vk::Sampler linearSampler);

	void record(vk::CommandBuffer cb, SyncScope& emission);
	void updateInputs(vk::ImageView emission);
	void initBuffers(const vk::Extent2D&);

protected:
	struct BloomLevel {
		vpp::ImageView view;
		vpp::TrDs ds;
	};

	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::Image tmpTarget_;
	std::vector<BloomLevel> tmpLevels_;
	std::vector<BloomLevel> emissionLevels_;
	vpp::ImageView fullBloom;
};

/// Constructs screen space reflection information from depth and normals.
class SSRPass {
public:
	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

	static constexpr auto format = vk::Format::r16g16b16a16Sfloat;

public:
	void init(const vpp::Device&);
	InitBufferData createBuffers(const vk::Extent2D&);
	void initBuffers(InitBufferData&);

	/// Inputs:
	/// - depth: used as shaderReadOnlyOptimal; sampled
	/// - normals: used as shaderReadOnlyOptimal; sampled
	void record(vk::CommandBuffer cb, SyncScope& depth,
		SyncScope& normals);
	void updateInputs(vk::ImageView depth, vk::ImageView normals);

	/// Outputs:
	/// - ssr target as shaderReadOnlyOptimal: (uv, blur, factor)
	const auto& target() const { return target_; }
	SyncScope targetScope();

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::ViewableImage target_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

/// Applies ambient occlusion and ibl.
/// NOTE: could use normals, albedo, depth as input attachments
///   must be passed in initBuffers already then.
class AOPass {
public:
	/// Inputs:
	/// - depth: used as shaderReadOnlyOptimal; sampled (for pos reconstruction)
	/// - normals: used as shaderReadOnlyOptimal; sampled
	/// - albedo: used as shaderReadOnlyOptimal; sampled
	/// Modifying:
	/// - light: used as general; storage image, applies ao
	void record(vk::CommandBuffer cb, SyncScope& depth,
		SyncScope& normals, SyncScope& ssao);
	void updateInputs(vk::ImageView depth, vk::ImageView normals,
		vk::ImageView ssao);

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};
