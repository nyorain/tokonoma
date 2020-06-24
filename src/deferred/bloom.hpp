#pragma once

#include "pass.hpp"
#include "lens.hpp"
#include <tkn/render.hpp>
#include <tkn/types.hpp>

using namespace tkn::types;

/// Bright-color-pass filter for bloom. Blurs the bloom/emission buffer
/// on multiple mip map levels.
/// Inputs:
/// - emission: used as transferSrc; blitted from
/// - light: used as shaderReadOnlyOptimal; sampled
/// Output:
/// - bloom (returned): transferSrc/general layout,
///   depending on the 'mipBlurred' setting.
///   Its imageView covers all bloom mip levels (fullView()).
class BloomPass {
public:
	// static constexpr vk::Format format = GeometryPass::emissionFormat;
	static constexpr vk::Format format = vk::Format::r16g16b16a16Sfloat;
	static constexpr unsigned groupDimSize = 16u;

	struct InitData {
		std::vector<vpp::TrDs::InitData> initTmpLevels;
		std::vector<vpp::TrDs::InitData> initTargetLevels;
		vpp::TrDs::InitData initFilterDs;
		vpp::SubBuffer::InitData initUbo;
	};

	struct InitBufferData {
		vpp::Image::InitData initTmpTarget;
		vpp::Image::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

	static constexpr u32 normManhattan = 1;
	static constexpr u32 normEuclid = 2;
	static constexpr u32 normMax = 3;

public:
	/// Influences how the bloom mipmap levels are generated.
	/// When true, will first blur, then create mipmaps. That results
	/// in a somewhat stronger overall blur for higher mipmap levels and
	/// a stronger bloom effect. When false, will first
	/// create all mipmap levels, then blur them all (independently).
	/// Rerecord needed when changed.
	bool mipBlurred = true;

	/// Maximum number of mipmap levels (will only be less if render
	/// buffer is too small, shouldn't be the case).
	/// Buffer recreation needed when changed.
	unsigned maxLevels = 3u;

	/// See bloom.comp
	struct {
		float highPassThreshold = 0.25f;
		float bloomPow = 0.8f;
		u32 norm = normEuclid;
	} params;

public:
	BloomPass() = default;
	void create(InitData&, const PassCreateInfo&);
	void init(InitData&, const PassCreateInfo&);

	void createBuffers(InitBufferData&, tkn::WorkBatcher&, const vk::Extent2D&);
	void initBuffers(InitBufferData&, vk::ImageView lightInput);

	void record(vk::CommandBuffer cb, vk::Image emission, vk::Extent2D);
	void updateDevice();

	SyncScope dstScopeEmission() const;
	SyncScope dstScopeLight() const;
	SyncScope srcScopeTarget() const;

	vk::ImageView fullView() const { return fullView_; }
	unsigned levelCount() const { return levelCount_; }
	const auto& target() const { return target_; }

protected:
	void recordBlur(vk::CommandBuffer, unsigned mip, vk::Extent2D);

	struct BloomLevel {
		vpp::ImageView view;
		vpp::TrDs ds;
	};

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDs ds;
		vpp::SubBuffer ubo;
		vpp::MemoryMapView uboMap;
	} filter_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} blur_;

	vpp::Image target_;
	std::vector<BloomLevel> targetLevels_;

	vpp::Image tmpTarget_; // for first step of blur
	std::vector<BloomLevel> tmpLevels_;

	vpp::ImageView fullView_;
	unsigned levelCount_;
};
