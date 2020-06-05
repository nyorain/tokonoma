#include <tkn/formats.hpp>
#include <tkn/f16.hpp>
#include <tkn/bits.hpp>
#include <vkpp/enums.hpp>
#include <vkpp/structs.hpp>

namespace tkn {

bool isHDR(vk::Format format) {
	// TODO: not sure about scaled formats, what are those?
	//  also what about packed formats? e.g. vk::Format::b10g11r11UfloatPack32?
	// TODO: even for snorm/unorm 16/32 bit formats we probably want to
	//  use the stbi hdr loader since otherwise we lose the precision
	//  when stbi converts to 8bit
	switch(format) {
		case vk::Format::r16Sfloat:
		case vk::Format::r16g16Sfloat:
		case vk::Format::r16g16b16Sfloat:
		case vk::Format::r16g16b16a16Sfloat:
		case vk::Format::r32Sfloat:
		case vk::Format::r32g32Sfloat:
		case vk::Format::r32g32b32Sfloat:
		case vk::Format::r32g32b32a32Sfloat:
		case vk::Format::r64Sfloat:
		case vk::Format::r64g64Sfloat:
		case vk::Format::r64g64b64Sfloat:
		case vk::Format::r64g64b64a64Sfloat:
			return true;
		default:
			return false;
	}
}

bool isSRGB(vk::Format format) {
	switch(format) {
		case vk::Format::r8Srgb:
		case vk::Format::r8g8Srgb:
		case vk::Format::r8g8b8Srgb:
		case vk::Format::r8g8b8a8Srgb:
		case vk::Format::b8g8r8a8Srgb:
		case vk::Format::b8g8r8Srgb:
		case vk::Format::a8b8g8r8SrgbPack32:
			return true;
		default:
			return false;
	}
}

vk::Format toggleSRGB(vk::Format format) {
	switch(format) {
		case vk::Format::r8Srgb:
			return vk::Format::r8Unorm;
		case vk::Format::r8g8Srgb:
			return vk::Format::r8g8Unorm;
		case vk::Format::r8g8b8Srgb:
			return vk::Format::r8g8b8Unorm;
		case vk::Format::r8g8b8a8Srgb:
			return vk::Format::r8g8b8a8Unorm;
		case vk::Format::b8g8r8a8Srgb:
			return vk::Format::b8g8r8a8Unorm;
		case vk::Format::b8g8r8Srgb:
			return vk::Format::b8g8r8Unorm;
		case vk::Format::a8b8g8r8SrgbPack32:
			return vk::Format::a8b8g8r8UnormPack32;

		case vk::Format::r8Unorm:
			return vk::Format::r8Srgb;
		case vk::Format::r8g8Unorm:
			return vk::Format::r8g8Srgb;
		case vk::Format::r8g8b8Unorm:
			return vk::Format::r8g8b8Srgb;
		case vk::Format::r8g8b8a8Unorm:
			return vk::Format::r8g8b8a8Srgb;
		case vk::Format::b8g8r8a8Unorm:
			return vk::Format::b8g8r8a8Srgb;
		case vk::Format::b8g8r8Unorm:
			return vk::Format::b8g8r8Srgb;
		case vk::Format::a8b8g8r8UnormPack32:
			return vk::Format::a8b8g8r8SrgbPack32;

		default: return format;
	}
}

vk::ImageType minImageType(vk::Extent3D size, unsigned minDim) {
	if(size.depth > 1 || minDim > 2) {
		return vk::ImageType::e3d;
	} else if(size.height > 1 || minDim > 1) {
		return vk::ImageType::e2d;
	} else {
		return vk::ImageType::e1d;
	}
}

// NOTE: even if size.y == 1, when cubemap is true, we will return
// cubemap view types (since there are no 1D cube types).
vk::ImageViewType minImageViewType(vk::Extent3D size, unsigned layers,
		bool cubemap, unsigned minDim) {
	if(size.depth > 1 || minDim > 2) {
		dlg_assertm(layers == 0 && cubemap == 0,
			"Layered or cube 3D images are not allowed");
		return vk::ImageViewType::e3d;
	}

	if(cubemap) {
		dlg_assert(layers % 6 == 0u);
		return (layers > 6 ? vk::ImageViewType::cubeArray : vk::ImageViewType::cube);
	}

	if(size.height > 1 || minDim > 1) {
		return layers > 1 ? vk::ImageViewType::e2dArray : vk::ImageViewType::e2d;
	} else {
		return layers > 1 ? vk::ImageViewType::e1dArray : vk::ImageViewType::e1d;
	}
}


nytl::Vec4d read(vk::Format srcFormat, nytl::Span<const std::byte>& src) {
	switch(srcFormat) {
		case vk::Format::r16Sfloat:
			return {
				float(read<f16>(src)),
				1.0,
				1.0,
				1.0
			};
		case vk::Format::r16g16Sfloat:
			return {
				float(read<f16>(src)),
				float(read<f16>(src)),
				1.0,
				1.0
			};
		case vk::Format::r16g16b16Sfloat:
			return {
				float(read<f16>(src)),
				float(read<f16>(src)),
				float(read<f16>(src)),
				1.0
			};
		default:
			throw std::logic_error("Format not supported for CPU reading");
	}
}

void write(vk::Format dstFormat, nytl::Span<std::byte>& dst, nytl::Vec4d color) {
	switch(dstFormat) {
		case vk::Format::r32Sfloat:
			write(dst, float(color[0]));
			break;
		case vk::Format::r32g32Sfloat:
			write(dst, Vec2f(color));
			break;
		case vk::Format::r32g32b32Sfloat:
			write(dst, Vec3f(color));
			break;
		case vk::Format::r32g32b32a32Sfloat:
			write(dst, Vec4f(color));
			break;

		case vk::Format::r64Sfloat:
			write(dst, double(color[0]));
			break;
		case vk::Format::r64g64Sfloat:
			write(dst, nytl::Vec2d(color));
			break;
		case vk::Format::r64g64b64Sfloat:
			write(dst, nytl::Vec3d(color));
			break;
		case vk::Format::r64g64b64a64Sfloat:
			write(dst, nytl::Vec4d(color));
			break;

		case vk::Format::r16Sfloat:
			write(dst, f16(color[0]));
			break;
		case vk::Format::r16g16Sfloat:
			write(dst, nytl::Vec2<f16>(color));
			break;
		case vk::Format::r16g16b16Sfloat:
			write(dst, nytl::Vec3<f16>(color));
			break;
		case vk::Format::r16g16b16a16Sfloat:
			write(dst, nytl::Vec4<f16>(color));
			break;
		default:
			throw std::logic_error("Format not supported for CPU writing");
	}
}

void convert(vk::Format dstFormat, nytl::Span<std::byte>& dst,
		vk::Format srcFormat, nytl::Span<const std::byte>& src) {
	auto col = read(srcFormat, src);
	write(dstFormat, dst, col);
}

} // namespace tkn
