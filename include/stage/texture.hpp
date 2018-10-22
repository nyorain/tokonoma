#pragma once

#include <vpp/image.hpp>
#include <nytl/stringParam.hpp>

namespace doi {

vpp::ViewableImage loadTexture(vpp::Device& dev, nytl::StringParam file);

} // namespace doi
