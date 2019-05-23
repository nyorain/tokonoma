// WIP: first concept of a pass abstraction
// or rather splitting the different passes in own modules
#pragma once

#include <stage/defer.hpp>
#include <stage/texture.hpp>

#include <stage/scene/scene.hpp> // only doi::Scene fwd needed
#include <stage/scene/light.hpp> // only doi::*Light fwd needed

#include <vpp/vk.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/handles.hpp>
#include <vpp/image.hpp>

struct RenderTarget {
	// Define when a resource was modified the last time.
	// Required when a pass wants to make those changes visible.
	vk::AccessFlags writeAccess {};
	vk::PipelineStageFlags writeStages {};

	// Define when a resource was read the last time, after being written.
	// Required when a pass itself modifies the resource, it must first
	// make sure that all previous accesses are finished. When writing
	// the resource, these flags should be reset.
	vk::AccessFlags readAccess {};
	vk::PipelineStageFlags readStages {};

	// the current logical layout.
	vk::ImageLayout layout = vk::ImageLayout::undefined;
	vk::Image image;
	vk::ImageView view; // optional
};

void transitionRead(vk::CommandBuffer cb, RenderTarget& target,
		vk::ImageLayout newLayout, vk::PipelineStageFlags stages,
		vk::AccessFlags access, vk::ImageSubresourceRange subres =
			{vk::ImageAspectBits::color, 0, 1, 0, 1}) {
	// in this case there already was a sufficient barrier in place
	if(target.layout == newLayout &&
			(target.readAccess & access) &&
			(target.readStages & stages)) {
		return;
	}

	vk::ImageMemoryBarrier barrier;
	barrier.image = target.image;
	barrier.oldLayout = target.layout;
	barrier.srcAccessMask = target.writeAccess;
	barrier.newLayout = newLayout;
	barrier.dstAccessMask = access;
	barrier.subresourceRange = subres;
	vk::cmdPipelineBarrier(cb, target.writeStages, stages,
		{}, {}, {}, {{barrier}});

	target.layout = newLayout;
	target.readStages |= stages;
	target.readAccess |= access;
}

void transitionWrite(vk::CommandBuffer cb, RenderTarget& target,
		vk::ImageLayout newLayout, vk::PipelineStageFlags stages,
		vk::AccessFlags access, vk::ImageSubresourceRange subres =
			{vk::ImageAspectBits::color, 0, 1, 0, 1}) {
	vk::ImageMemoryBarrier barrier;
	barrier.image = target.image;
	barrier.oldLayout = target.layout;
	barrier.srcAccessMask = target.writeAccess | target.readAccess;
	barrier.newLayout = newLayout;
	barrier.dstAccessMask = access;
	barrier.subresourceRange = subres;
	vk::cmdPipelineBarrier(cb, target.writeStages | target.readStages, stages,
		{}, {}, {}, {{barrier}});

	target.layout = newLayout;
	target.readStages = {};
	target.readAccess = {};
	target.writeStages = stages;
	target.writeAccess = access;
}

struct PassCreateInfo {
	const doi::WorkBatcher& wb;
	vk::Format depthFormat;

	struct {
		const vpp::TrDsLayout& scene;
		const vpp::TrDsLayout& material;
		const vpp::TrDsLayout& primitive;
		const vpp::TrDsLayout& light;
	} dsLayouts;

	struct {
		vk::Sampler linear;
		vk::Sampler nearest;
	} samplers;
};

/*
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
	SSAOPass() = default;
	SSAOPass(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);

	InitBufferData createBuffers(const vk::Extent2D&, const doi::WorkBatcher&);
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
	struct InitData {
		vpp::TrDs::InitData initDsHorz;
		vpp::TrDs::InitData initDsVert;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	SSAOBlurPass() = default;
	SSAOBlurPass(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&, vk::RenderPass ssaorp);

	void createBuffers(InitBufferData&, const vk::Extent2D&,
		const doi::WorkBatcher&);
	void initBuffers(InitBufferData&);

	void record(vk::CommandBuffer cb,
		SyncScope& depthScope, SyncScope& ssaoScope);
	void updateInputs(vk::ImageView depth, vk::ImageView ssao,
		vk::Framebuffer ssaofb);

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

class GeometryPass {
public:
	static constexpr auto normalsFormat = vk::Format::r16g16b16a16Snorm;
	static constexpr auto albedoFormat = vk::Format::r8g8b8a8Srgb;
	static constexpr auto emissionFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto linearDepthFormat = vk::Format::r16Sfloat;

	struct InitTarget {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

	struct InitBufferData {
		InitTarget initNormals;
		InitTarget initAlbedo;
		InitTarget initDepth;
		InitTarget initEmission;
	};

public:
	GeometryPass() = default;
	GeometryPass(const PassCreateInfo&);

	void createBuffers(InitBufferData&, const vk::Extent2D&,
		const doi::WorkBatcher&);
	void initBuffers(InitBufferData&, vk::ImageView depth);

	// Outputs:
	// - linear depth buffer (color, linear sampleable, reduced size)
	// - normal buffer (+ roughness)
	// - emission buffer (+ metallic)
	// - albedo buffer
	// - [depth buffer, only used internally; use linear depth buffer]
	void record(vk::CommandBuffer cb, const doi::Scene& scene);

	const auto& normalTarget() const { return normal_; }
	const auto& linearDepthTarget() const { return depth_; }
	const auto& emissionTarget() const { return emission_; }
	const auto& albedoTarget() const { return albedo_; }

	SyncScope normalScope() const;
	SyncScope linearDepthScope() const;
	SyncScope emissionScope() const;
	SyncScope albedoScope() const;

protected:
	vpp::RenderPass pass_;
	vpp::Framebuffer fb_;
	vpp::ViewableImage normal_;
	vpp::ViewableImage albedo_;
	vpp::ViewableImage emission_;
	vpp::ViewableImage depth_; // color, for linear sampling/mipmaps
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

class LightPass {
public:
	static constexpr auto format = vk::Format::r16g16b16a16Sfloat;

	struct InitData {
		vpp::TrDs::InitData initDs;
	};

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	LightPass() = default;
	LightPass(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo);

	void createBuffers(InitBufferData&, const vk::Extent2D&,
		const doi::WorkBatcher&);
	void initBuffers(InitBufferData&, const doi::WorkBatcher&);

	// Inputs:
	// - linear depth buffer
	// - normal buffer (+ roughness)
	// - emission buffer (+ metallic)
	// - albedo buffer
	// Outputs:
	// - light buffer (hdr), contains shaded scene
	void record(vk::CommandBuffer cb,
		vpp::BufferSpan boxIndices,
		SyncScope& depthScope, SyncScope& normalScope,
		SyncScope& albedoScope, SyncScope& emissionScope,
		nytl::Span<const doi::PointLight>,
		nytl::Span<const doi::DirLight>);
	void updateInputs(vk::ImageView depth, vk::ImageView normal,
		vk::ImageView albedo, vk::ImageView emissionScope);

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

/// Constructs screen space reflection information from depth and normals.
class SSRPass {
public:
	static constexpr auto format = vk::Format::r16g16b16a16Sfloat;

	struct InitBufferData {
		vpp::ViewableImage::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

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
	constexpr static vk::Format format = LightPass::format;

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
*/
