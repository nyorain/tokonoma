#pragma once

#include <tkn/passes/blur.hpp>
#include <tkn/render.hpp>
#include <tkn/types.hpp>

namespace tkn {

/// BloomPass working directly on the multi-level image of a HighlightPass.
class BloomPass {
public:
	static constexpr vk::Format format = vk::Format::r16g16b16a16Sfloat;
	static constexpr unsigned groupDimSize = 16u;

	struct InitBufferData {
		std::vector<GaussianBlur::InstanceInitData> initBlurs;
		vpp::Image::InitData initTmpTarget;
		vpp::Image::InitData initTarget;
		vk::ImageViewCreateInfo viewInfo;
	};

public:
	/// Influences how the bloom mipmap levels are generated.
	/// When true, will first blur, then create mipmaps. That results
	/// in a somewhat stronger overall blur for higher mipmap levels and
	/// a stronger bloom effect. When false, will first
	/// create all mipmap levels, then blur them all (independently).
	/// Rerecord needed when changed.
	bool mipBlurred = true;

	/// Blur kernel parameters, see GaussianBlur::createKernel
	unsigned blurHSize = 24;
	float blurFac = 1.5;

public:
	BloomPass() = default;

	void createBuffers(InitBufferData&, tkn::WorkBatcher&, unsigned levels,
		vk::Extent2D, const GaussianBlur& blur);
	void initBuffers(InitBufferData&, vk::Image highlightImage,
		vk::ImageView highlightView, const GaussianBlur& blur);
	void record(vk::CommandBuffer, vk::Image highlight, vk::Extent2D,
		const GaussianBlur& blur);

	SyncScope dstScopeHighlight() const; // only first level
	SyncScope srcScopeHighlight() const; // all levels

	vk::ImageView fullView() const { return fullView_; }
	unsigned levelCount() const { return levelCount_; }

protected:
	struct Level {
		GaussianBlur::Instance blur;
		vpp::ImageView target;
		vpp::ImageView tmp;
	};

	std::vector<Level> levels_;
	vpp::Image tmpTarget_; // for first step of separated blur
	vpp::ImageView fullView_; // needed for rendering in the end
	unsigned levelCount_; // number of blur levels
};

} // namespace tkn
