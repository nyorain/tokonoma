#pragma once

#include <tkn/types.hpp>
#include <vkpp/fwd.hpp>

namespace tkn {

// NOTE: at the moment, these functions support only a small number
// of formats. Just extend them with whatever is needed.

nytl::Vec4d read(vk::Format srcFormat, nytl::Span<const std::byte>& src);
void write(vk::Format dstFormat, nytl::Span<std::byte>& dst, nytl::Vec4d color);
void convert(vk::Format dstFormat, nytl::Span<std::byte>& dst,
		vk::Format srcFormat, nytl::Span<const std::byte>& src);

} // namespact tkn
