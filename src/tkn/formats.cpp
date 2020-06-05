#include <tkn/formats.hpp>
#include <tkn/f16.hpp>
#include <tkn/bits.hpp>
#include <vkpp/enums.hpp>

namespace tkn {

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
