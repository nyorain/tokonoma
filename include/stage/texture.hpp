#pragma once

#include <vpp/image.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>

namespace doi {

// NOTE: assumes non-hdr data to be in srgb color space (which most data is).

// TODO: allow 8bit textures (r8)
// TODO: use 16 bit float formats for images (requires blitting and stage images)
// TODO: maybe also allow to choose whether data should be srgb
// enum class Type {
//  r8,
// 	rgba8,
// 	rgba16f,
// 	rgba32f
// };

vpp::ViewableImage loadTexture(const vpp::Device& dev, nytl::StringParam file,
	bool hdr = false);
vpp::ViewableImage loadTexture(const vpp::Device& dev, vk::Extent3D size,
	vk::Format format, nytl::Span<const std::byte> data);

// creates an array of textures
vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
	nytl::Span<const nytl::StringParam> files, bool cubemap = false,
	bool hdr = false);


} // namespace doi
