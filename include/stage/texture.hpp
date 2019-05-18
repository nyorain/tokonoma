#pragma once

#include <stage/defer.hpp>
#include <stage/image.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>
#include <nytl/vec.hpp>

// NOTE: none of the functions/classes have support for compressed/packed
// or depth/stencil formats

namespace doi {

// NOTE: stb offers a function to autodetect whether the image is hdr.
// We don't use this here (e.g. use a optional<bool> hdr parameter)
// since the caller knows what it wants the texture for and has to adapt
// it's pipelines (so getting images that return sampled values >1 would
// be unexpected).
// If a pipeline supports and expects both hdr and ldr images properly,
// the caller can still use stbi_is_hdr(filename) manually before calling this.
// The default format used when 'hdr = true' is passed is rgba16f, otherwise
// will use rgba8.

// returns whether the given format an hold high dynamic range data, i.e.
// has a float format and can store values >1.0.
bool isHDR(vk::Format);
bool isSRGB(vk::Format);
vk::Format toggleSRGB(vk::Format);

// Vulkan enums declared via static to not include vulkan headers
struct TextureCreateParams {
	// default usage flags: sampled, storage, input attachment, transfer src/dst
	static const vk::ImageUsageFlags defaultUsage;
	static const vk::Format defaultFormat; // r8g8b8a8Srgb

	// The format of the created image.
	// depending on whether format is srgb the given files data will be
	// interpreted as srgb (only for overloads that don't explicitly
	// specify dataFormat).
	// TODO: allow to just use the format of the image data?
	// but then the the caller has to know the resulting format somehow,
	// store it?
	vk::Format format = defaultFormat;

	// If given, will force interpret the data as srgb/non-srgb.
	// No effect if the data is not unorm.
	std::optional<bool> srgb {};

	// Usage flags of the created image.
	vk::ImageUsageFlags usage = defaultUsage;

	// number of mipmap levels to create
	// none, auto: create as many mipmaps as the image data provides
	// Otherwise creates the specified number of mipmap levels.
	// Specify 0 to create a full mipmap chain.
	std::optional<unsigned> mipLevels {};

	// only relevant if mipmaps > 1
	// none, auto: only fill mimpmaps if image provides it
	// false: don't fill any mipmaps levels
	// true: use mipmap levels from image data but if there are none,
	//   create them manually (via vulkan blits)
	std::optional<bool> fillMipmaps {};

	// Whether to create a cubemap image and view.
	// If true, there has to be 6 faces in the provided image and
	// layerCount in ViewRange must be 6 (or 0).
	// If false, all additional faces in the provided image are
	// ignored.
	bool cubemap {};

	struct ViewRange {
		unsigned baseMipLevel = 0;
		unsigned baseArrayLayer = 0;
		unsigned layerCount = 0; // 0: all
		unsigned levelCount = 0; // 0: all
	} view;
};

// Basically just a ViewableImage that allows for deferred uploading.
// Always allocated on deviceLocal memory.
class Texture {
public:
	struct InitData {
		vpp::ViewableImage::InitData initImage {};

		vpp::SubBuffer stageBuf;
		vpp::SubBuffer::InitData initStageBuf {};

		vpp::Image stageImage;
		vpp::Image::InitData initStageImage {};

		vk::Format dstFormat;
		std::unique_ptr<ImageProvider> image;
		vk::Format dataFormat; // may have toggles srgb
		unsigned levels {}; // how many levels in total, >0
		unsigned layers {}; // how many layers/faces in total, >0
		unsigned fillLevels {}; // how many pre-filled levels, >0
		bool genLevels {}; // whether to generate remaining levels
		bool cubemap {}; // whether image is cubemap

		TextureCreateParams::ViewRange view;
	};

public:
	Texture() = default;
	Texture(vpp::ViewableImage&& img) : image_(std::move(img)) {}

	Texture(const vpp::Device& dev, std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& = {});
	Texture(const WorkBatcher&, std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& = {});
	Texture(InitData&, const WorkBatcher&, std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& = {});

	void init(InitData&, const WorkBatcher&);

	auto& device() const { return image_.device(); }
	auto& viewableImage() { return image_; }
	const auto& viewableImage() const { return image_; }
	const auto& image() const { return image_.image(); }
	const auto& imageView() const { return image_.imageView(); }
	const vk::ImageView& vkImageView() const { return image_.vkImageView(); }
	const vk::Image& vkImage() const { return image_.vkImage(); }

protected:
	vpp::ViewableImage image_;
};

} // namespace doi
