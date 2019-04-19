#pragma once

#include <vpp/image.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>

namespace doi {

// NOTE: assumes all data to be in srgb color space (which most data is).
// When read in shader, will return color in linear space.

// TODO: util for loading skyboxes. Wrapper/abstraction of loadTextureArray
// TODO: allow 8bit textures (r8)

vpp::ViewableImage loadTexture(const vpp::Device& dev, nytl::StringParam file);
vpp::ViewableImage loadTexture(const vpp::Device& dev, vk::Extent3D size,
	vk::Format format, nytl::Span<const std::byte> data);

// creates an array of textures
vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
	nytl::Span<const nytl::StringParam> files);


} // namespace doi
