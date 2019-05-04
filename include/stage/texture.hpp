#pragma once

#include <stage/defer.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>
#include <nytl/vec.hpp>

// all functions only work for color only images
// TODO: support additional usage flags instead of always assuming
// pretty much everything

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

// Wrapper around stb load image function.
// The returned datas format depends on the given channels and hdr but is
// tightly packed.
std::tuple<std::unique_ptr<std::byte[]>, vk::Extent2D> loadImage(
	nytl::StringParam filename, unsigned channels = 4, bool hdr = false);

// If an explicit format for the texture is given, it must be possible
// to blit into it with vulkan (vulkan does not allow this for all
// format conversions).
vpp::ViewableImage loadTexture(const vpp::Device& dev, nytl::StringParam file,
	bool srgb = true, bool hdr = false, bool mipmap = true);
vpp::ViewableImage loadTexture(const vpp::Device& dev, nytl::StringParam file,
	vk::Format format, bool inputSrgb = false, bool mipmap = true);
vpp::ViewableImage loadTexture(const vpp::Device& dev, vk::Extent3D size,
	vk::Format format, nytl::Span<const std::byte> data,
	vk::Format dataFormat = {}, bool mipmap = true);

// TODO: support mipmaps here as well
// TODO: overload that takes data and sizes
// creates an array of textures/a cubemap
vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
	nytl::Span<const nytl::StringParam> files, bool cubemap = false,
	bool srgb = true, bool hdr = false);
vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
	nytl::Span<const nytl::StringParam> files, vk::Format format,
	bool cubemap = false, bool inputSrgb = false);

// returns whether the given format an hold high dynamic range data, i.e.
// has a float format and can store values >1.0.
bool isHDR(vk::Format);
bool isSRGB(vk::Format);

// computes the number of mipmap levels needed for a full mipmap chain
// for an image of the given size.
unsigned mipmapLevels(const vk::Extent2D& extent);

// Vulkan enums declared via static to not include vulkan headers
struct TextureCreateParams {
	// default usage flags: sampled, storage, input attachment, transfer src/dst
	static const vk::ImageUsageFlags defaultUsage;
	static const vk::Format defaultFormat; // r8g8b8a8Srgb

	// depending on whether format is srgb the given files data will be
	// interpreted as srgb (only for overloads that don't explicitly
	// specify dataFormat).
	vk::Format format = defaultFormat;
	vk::ImageUsageFlags usage = defaultUsage;
	bool mipmaps = true;
	bool cubemap = false; // only for array textures
};

// Basically just a ViewableImage that allows for deferred uploading.
// Always allocated on deviceLocal memory.
class Texture {
public:
	struct InitData {
		vpp::SubBuffer stageBuf;
		vpp::Image stageImage;
		vk::Format dstFormat;
		nytl::Vec2ui size;
		std::vector<std::unique_ptr<std::byte[]>> data;
		vk::Format dataFormat;
		unsigned levels {1};
		bool cubemap {};
	};

public:
	Texture() = default;
	Texture(vpp::ViewableImage&& img) : image_(std::move(img)) {}

	Texture(const vpp::Device& dev, nytl::StringParam file,
		const TextureCreateParams& = {});
	Texture(const vpp::Device& dev, nytl::Span<const char* const> files,
		const TextureCreateParams& = {});
	Texture(const vpp::Device&,
		std::vector<std::unique_ptr<std::byte[]>> dataLayers,
		vk::Format dataFormat, const vk::Extent2D& size,
		const TextureCreateParams& = {});

	Texture(const WorkBatcher&, InitData&, nytl::StringParam file,
		const TextureCreateParams& = {});
	Texture(const WorkBatcher&, InitData&, nytl::Span<const char* const> files,
		const TextureCreateParams& = {});
	Texture(const WorkBatcher&, InitData&,
		std::vector<std::unique_ptr<std::byte[]>> dataLayers,
		vk::Format dataFormat, const vk::Extent2D& size,
		const TextureCreateParams& = {});

	void initAlloc(const WorkBatcher&, InitData&);
	void initFinish(const WorkBatcher&, InitData&);

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
