#pragma once

#include <vpp/image.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>
#include <optional>

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
// tightly packed. The depth component of the extent is always 1.
// The returned data must be freed. TODO: we could return a custom unique_ptr
std::tuple<std::byte*, vk::Extent3D> loadImage(nytl::StringParam filename,
	unsigned channels = 4, bool hdr = false);

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

// computes the number of mipmap levels needed for a full mipmap chain
// for an image of the given size.
unsigned mipmapLevels(const vk::Extent2D& extent);

} // namespace doi
