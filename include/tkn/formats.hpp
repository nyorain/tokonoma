#pragma once

#include <tkn/types.hpp>
#include <nytl/vec.hpp>
#include <vkpp/fwd.hpp>

// NOTE: at the moment, these functions support only a small number
// of formats. Just extend them with whatever is needed.

namespace tkn {

// Returns whether the given format an hold high dynamic range data, i.e.
// has a float format and can store values >1.0.
bool isHDR(vk::Format);
bool isSRGB(vk::Format);
vk::Format toggleSRGB(vk::Format);
vk::ImageType minImageType(vk::Extent3D, unsigned minDim = 1u);
vk::ImageViewType minImageViewType(vk::Extent3D, unsigned layers,
	bool cubemap, unsigned minDim = 1u);

// NOTE: rgb must be linear
u32 e5b9g9r9FromRgb(nytl::Vec3f rgb);
nytl::Vec3f e5b9g9r9ToRgb(u32 e5r9g9b9);

// CPU format conversion. This is needed to support reading and writing of
// data in formats that the GPU does not support.

nytl::Vec4d read(vk::Format srcFormat, nytl::Span<const std::byte>& src);
void write(vk::Format dstFormat, nytl::Span<std::byte>& dst, nytl::Vec4d color);
void convert(vk::Format dstFormat, nytl::Span<std::byte>& dst,
		vk::Format srcFormat, nytl::Span<const std::byte>& src);

} // namespact tkn
