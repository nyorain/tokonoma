#pragma once

#include <vpp/image.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>

namespace doi {

// TODO: util for loading skyboxes. Wrapper/abstraction of loadTextureArray
// TODO: allow 8bit textures (r8)

vpp::ViewableImage loadTexture(vpp::Device& dev, nytl::StringParam file);
vpp::ViewableImage loadTextureArray(vpp::Device& dev,
	nytl::Span<const nytl::StringParam> files);

} // namespace doi
