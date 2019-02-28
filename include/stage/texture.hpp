#pragma once

#include <vpp/image.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>

namespace doi {

vpp::ViewableImage loadTexture(vpp::Device& dev, nytl::StringParam file);
vpp::ViewableImage loadTextureArray(vpp::Device& dev,
	nytl::Span<nytl::StringParam> files);

} // namespace doi
