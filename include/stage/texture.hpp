#pragma once

#include <vpp/image.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>
#include <optional>

// all functions only work for color only images

namespace doi {

// TODO: support for compressed formats, low priority for now

// TODO: allow 8bit textures (r8)
// TODO: use 16 bit float formats for images (requires blitting and stage images)
// enum class TextureFormat {
//  	r8,
// 	rgba8,
// 	rgba16f,
// 	rgba32f
// };
//
// struct TextureCreateInfo {
// 	// will output warning if type and image data differ in significant ways,
// 	// e.g. image data is hdr this is an ldr format or the other way around
// 	TextureFormat format {TextureFormat::rgba8};
//
// 	// when format is hdr, this must be false
// 	// for ldr formats, will create a unorm image if this is false (useful
// 	// e.g. for normal textures)
// 	bool srgb {true};
//
// 	// how many mipmap levels to create
// 	// number of levels will be 1 + ceil(log2(max(width, height))),
// 	// as used in opengl per default
// 	bool mipap {true};
// };

// NOTE: stb offers a function to autodetect whether the image is hdr.
// We don't use this here (e.g. use a optional<bool> hdr parameter)
// since the caller knows what it wants the texture for and has to adapt
// it's pipelines (so getting images that return sampled values >1 would
// be unexpected).
// If a pipeline supports and expects both hdr and ldr images properly,
// the caller can still use stbi_is_hdr(filename) manually before calling this.
//
// The default format used when 'hdr = true' is passed is rgba16f, otherwise
// will use rgba8.

// Wrapper around stb load image function.
// The returned datas format depends on the given channels and hdr but is
// tightly packed. The depth component of the extent is always 1.
// The returned data must be freed. TODO: we could use a custom unique_ptr
std::tuple<std::byte*, vk::Extent3D> loadImage(nytl::StringParam filename,
	unsigned channels = 4, bool hdr = false);

vpp::ViewableImage loadTexture(const vpp::Device& dev, nytl::StringParam file,
	bool srgb = true, bool hdr = false);
vpp::ViewableImage loadTexture(const vpp::Device& dev, nytl::StringParam file,
	vk::Format format, bool inputSrgb = false);
vpp::ViewableImage loadTexture(const vpp::Device& dev, vk::Extent3D size,
	vk::Format format, nytl::Span<const std::byte> data,
	vk::Format dataFormat = {});

// creates an array of textures
vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
	nytl::Span<const nytl::StringParam> files, bool srgb = true,
	bool hdr = false, bool cubemap = false);
vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
	nytl::Span<const nytl::StringParam> files, vk::Format format,
	bool cubemap = false, bool inputSrgb = false);

bool isHDR(vk::Format);

} // namespace doi
