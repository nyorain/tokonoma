#include <tkn/image.hpp>
#include <tkn/stream.hpp>
#include <tkn/util.hpp>
#include <tkn/f16.hpp>
#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <nytl/scope.hpp>
#include <vector>

#define TINYEXR_IMPLEMENTATION
#include <zlib.h>
#include "tinyexr.hpp"

// TODO: extend support, evaluate what is needed/useful:
// - support for tiled/multipart images?
//   could be useful for cubemaps/mip levels
// - support for deep images?
// TODO: remove unneeded high-level functions from tinyexr.
//   should reduce it by quite some size.

// technical source/specificiation:
//   https://www.openexr.com/documentation/TechnicalIntroduction.pdf
// lots of good example images:
//   https://github.com/AcademySoftwareFoundation/openexr-images
// Supporting all images in MultiResolution shouldn't be too hard.
// We already support MultiView (the rgb ones)

namespace tkn {

ReadError toReadError(int res) {
	switch(res) {
		case TINYEXR_SUCCESS:
			return ReadError::none;
		case TINYEXR_ERROR_INVALID_MAGIC_NUMBER:
		case TINYEXR_ERROR_INVALID_EXR_VERSION:
		case TINYEXR_ERROR_INVALID_FILE:
		case TINYEXR_ERROR_INVALID_DATA:
		case TINYEXR_ERROR_INVALID_HEADER:
			return ReadError::invalidType;
		case TINYEXR_ERROR_UNSUPPORTED_FORMAT:
			return ReadError::unsupportedFormat;
		case TINYEXR_ERROR_UNSUPPORTED_FEATURE:
			return ReadError::cantRepresent;
		default:
			return ReadError::internal;
	}
}

// NOTE: we return formats in order RGBA. We could also return ABGR or
// any other order but RGBA is probably the most common and widest
// supported one.

constexpr auto noChannel = u32(0xFFFFFFFF);
vk::Format parseFormat(const std::array<u32, 4>& mapping, int exrPixelType,
		bool forceRGBA) {
	auto maxChan = forceRGBA ? 3 :
		mapping[3] != noChannel ? 3 :
		mapping[2] != noChannel ? 2 :
		mapping[1] != noChannel ? 1 : 0;
	switch(maxChan) {
		case 0:
			switch(exrPixelType) {
				case TINYEXR_PIXELTYPE_UINT: return vk::Format::r32Uint;
				case TINYEXR_PIXELTYPE_HALF: return vk::Format::r16Sfloat;
				case TINYEXR_PIXELTYPE_FLOAT: return vk::Format::r32Sfloat;
				default: return vk::Format::undefined;
			}
		case 1:
			switch(exrPixelType) {
				case TINYEXR_PIXELTYPE_UINT: return vk::Format::r32g32Uint;
				case TINYEXR_PIXELTYPE_HALF: return vk::Format::r16g16Sfloat;
				case TINYEXR_PIXELTYPE_FLOAT: return vk::Format::r32g32Sfloat;
				default: return vk::Format::undefined;
			}
		case 2:
			switch(exrPixelType) {
				case TINYEXR_PIXELTYPE_UINT: return vk::Format::r32g32b32Uint;
				case TINYEXR_PIXELTYPE_HALF: return vk::Format::r16g16b16Sfloat;
				case TINYEXR_PIXELTYPE_FLOAT: return vk::Format::r32g32b32Sfloat;
				default: return vk::Format::undefined;
			}
		case 3:
			switch(exrPixelType) {
				case TINYEXR_PIXELTYPE_UINT: return vk::Format::r32g32b32a32Uint;
				case TINYEXR_PIXELTYPE_HALF: return vk::Format::r16g16b16a16Sfloat;
				case TINYEXR_PIXELTYPE_FLOAT: return vk::Format::r32g32b32a32Sfloat;
				default: return vk::Format::undefined;
			}
		default: return vk::Format::undefined;
	}
}

ReadError readExr(std::unique_ptr<Stream>&& stream,
		std::unique_ptr<ImageProvider>& provider, bool forceRGBA) {

	// TODO: could optimize this significantly.
	// When it's a memory stream, just use the memory.
	// When it's a file stream and we're on linux, try to map it.
	stream->seek(0, Stream::SeekOrigin::end);
	auto size = stream->address();
	stream->seek(0);

	std::unique_ptr<std::byte[]> buf = std::make_unique<std::byte[]>(size);
	auto* data = reinterpret_cast<const unsigned char*>(buf.get());
	stream->read(buf.get(), size);

	EXRVersion version;
	auto res = ParseEXRVersionFromMemory(&version, data, size);
	if(res != TINYEXR_SUCCESS) {
		dlg_debug("ParseEXRVersionFromMemory: {}", res);
		return toReadError(res);
	}

	dlg_info("EXR image information:");
	dlg_info("  version: {}", version.version);
	dlg_info("  tiled: {}", version.tiled);
	dlg_info("  long_name: {}", version.long_name);
	dlg_info("  non_image: {}", version.non_image);
	dlg_info("  multipart: {}", version.multipart);

	if(version.multipart || version.tiled || version.non_image) {
		return ReadError::cantRepresent;
	}

	const char* err {};
	EXRHeader header {};
	res = ParseEXRHeaderFromMemory(&header, &version, data, size, &err);
	if(res != TINYEXR_SUCCESS) {
		dlg_debug("ParseEXRHeaderFrommemory: {} ({})", err ? err : "-", res);
		FreeEXRErrorMessage(err);
		return toReadError(res);
	}

	auto headerGuard = nytl::ScopeGuard([&]{ FreeEXRHeader(&header); });
	for(auto i = 0u; i < unsigned(header.num_custom_attributes); ++i) {
		auto& att = header.custom_attributes[i];
		dlg_debug("attribute {} (type {}, size {})", att.name, att.type, att.size);
	}

	struct Layer {
		std::string_view name;
		std::array<u32, 4> mapping {noChannel, noChannel, noChannel, noChannel};
	};

	std::vector<Layer> layers;
	std::optional<int> oPixelType;

	for(auto i = 0u; i < unsigned(header.num_channels); ++i) {
		std::string_view name = header.channels[i].name;
		dlg_debug("channel {}: {}", i, name);

		std::string_view layerName, channelName;
		auto sepos = name.find_last_of('.');
		if(sepos == std::string::npos) {
			// default layer.
			// This means we will interpret ".R" and "R" the same,
			// an image that has both can't be parsed.
			layerName = "";
			channelName = name;
		} else {
			std::tie(layerName, channelName) = split(name, sepos);
		}

		unsigned id;
		if(channelName == "R") id = 0;
		else if(channelName == "G") id = 1;
		else if(channelName == "B") id = 2;
		else if(channelName == "A") id = 3;
		else {
			dlg_info(" >> Ignoring unknown channel {}", channelName);
			continue;
		}

		auto it = std::find_if(layers.begin(), layers.end(),
			[&](auto& layer) { return layer.name == layerName; });
		if(it == layers.end()) {
			layers.emplace_back().name = layerName;
			it = layers.end() - 1;
		}

		if(it->mapping[id] != noChannel) {
			dlg_warn("EXR layer has multiple {} channels", name);
			return ReadError::unsupportedFormat;
		}

		it->mapping[id] = i;
		if(!oPixelType) {
			oPixelType = header.channels[i].pixel_type;
		} else {
			// all known (rgba) channels must have the same pixel type,
			// there are no formats with varying types.
			if(header.channels[i].pixel_type != *oPixelType) {
				dlg_warn("EXR image channels have different pixel types");
				return ReadError::unsupportedFormat;
			}
		}
	}

	if(layers.empty()) {
		dlg_error("EXR image has no channels/layers");
		return ReadError::empty;
	}

	auto pixelType = *oPixelType;

	std::optional<vk::Format> oFormat;
	for(auto it = layers.begin(); it != layers.end();) {
		auto iformat = parseFormat(it->mapping, pixelType, forceRGBA);
		if((oFormat && *oFormat != iformat) || iformat == vk::Format::undefined) {
			dlg_warn("EXR image layer {} has {} format, ignoring it",
				oFormat ? "different" : "invalid", it - layers.begin());
			it = layers.erase(it);
			continue;
		}

		oFormat = iformat;
		++it;
	}

	if(!oFormat) {
		dlg_warn("EXR image has no layer with parsable format");
		return ReadError::empty;
	}

	auto format = *oFormat;

	EXRImage image {};
	res = LoadEXRImageFromMemory(&image, &header, data, size, &err);
	if(res != TINYEXR_SUCCESS) {
		dlg_debug("LoadEXRImageFromMemory: {} ({})", err ? err : "-", res);
		FreeEXRErrorMessage(err);
		return toReadError(res);
	}

	auto imageGuard = nytl::ScopeGuard([&]{ FreeEXRImage(&image); });
	if(!image.images) { // we already checked it's not in tiled format?!
		dlg_warn("Reading EXRImage returned no data");
		return ReadError::internal;
	}

	unsigned width = image.width;
	unsigned height = image.height;
	dlg_assert(image.num_channels == header.num_channels);

	// interlace channels
	auto layerSize = width * height * vpp::formatSize(format);
	auto totalSize = layers.size() * layerSize;
	auto interlaced = std::make_unique<std::byte[]>(totalSize);

	auto chanSize = pixelType == TINYEXR_PIXELTYPE_HALF ?  2 : 4;
	auto pixelSize = vpp::formatSize(format);

	std::byte src1[4];
	if(pixelType == TINYEXR_PIXELTYPE_HALF) {
		auto src = f16(1.f);
		std::memcpy(src1, &src, sizeof(src));
	} else if(pixelType == TINYEXR_PIXELTYPE_UINT) {
		auto src = u32(1);
		std::memcpy(src1, &src, sizeof(src));
	} else if(pixelType == TINYEXR_PIXELTYPE_FLOAT) {
		auto src = float(1.f);
		std::memcpy(src1, &src, sizeof(src));
	}

	// NOTE: instead of just forceRGBA, we could already perform
	// cpu format conversion here if this is desired (i.e. the required
	// format different than what we desire). Could extent the api
	// to allow this.
	for(auto l = 0u; l < layers.size(); ++l) {
		auto& layer = layers[l];

		for(auto y = 0u; y < height; ++y) {
			for(auto x = 0u; x < width; ++x) {
				auto address = y * width + x;
				auto laddress = l * width * height + address;
				auto dst = interlaced.get() + pixelSize * laddress;

				for(auto c = 0u; c < 4u; ++c) {
					auto id = layer.mapping[c];
					if(id == noChannel) {
						std::memcpy(dst + c * chanSize, src1, chanSize);
					} else {
						auto src = image.images[id] + chanSize * address;
						std::memcpy(dst + c * chanSize, src, chanSize);
					}
				}
			}
		}
	}


	provider = wrap({width, height, 1u}, format, 1, layers.size(), std::move(interlaced));
	return provider ? ReadError::none : ReadError::internal;
}

WriteError writeExr(nytl::StringParam path, const ImageProvider& provider) {
	auto [width, height, depth] = provider.size();
	if(depth > 1) {
		dlg_warn("writeExr: discarding {} slices", depth - 1);
	}

	if(provider.mipLevels() > 1) {
		dlg_warn("writeExr: discarding {} mips", provider.mipLevels() - 1);
	}

	// TODO: we could add support for writing multiple layers
	if(provider.layers() > 1) {
		dlg_warn("writeExr: discarding {} layers", provider.layers() - 1);
	}

	EXRChannelInfo channels[4] {};

	auto fmt = provider.format();
	auto nc = 0u;
	unsigned chanSize;
	int pixType;
	switch(fmt) {
		case vk::Format::r16g16b16a16Sfloat:
			std::strcpy(channels[nc++].name, "A"); [[fallthrough]];
		case vk::Format::r16g16b16Sfloat:
			std::strcpy(channels[nc++].name, "B"); [[fallthrough]];
		case vk::Format::r16g16Sfloat:
			std::strcpy(channels[nc++].name, "G"); [[fallthrough]];
		case vk::Format::r16Sfloat:
			chanSize = 2;
			pixType = TINYEXR_PIXELTYPE_HALF;
			std::strcpy(channels[nc++].name, "R");
			break;

		case vk::Format::r32g32b32a32Sfloat:
			std::strcpy(channels[nc++].name, "A"); [[fallthrough]];
		case vk::Format::r32g32b32Sfloat:
			std::strcpy(channels[nc++].name, "B"); [[fallthrough]];
		case vk::Format::r32g32Sfloat:
			std::strcpy(channels[nc++].name, "G"); [[fallthrough]];
		case vk::Format::r32Sfloat:
			chanSize = 4;
			pixType = TINYEXR_PIXELTYPE_FLOAT;
			std::strcpy(channels[nc++].name, "R");
			break;

		case vk::Format::r32g32b32a32Uint:
			std::strcpy(channels[nc++].name, "A"); [[fallthrough]];
		case vk::Format::r32g32b32Uint:
			std::strcpy(channels[nc++].name, "B"); [[fallthrough]];
		case vk::Format::r32g32Uint:
			std::strcpy(channels[nc++].name, "G"); [[fallthrough]];
		case vk::Format::r32Uint:
			chanSize = 4;
			pixType = TINYEXR_PIXELTYPE_UINT;
			std::strcpy(channels[nc++].name, "R");
			break;

		default:
			dlg_error("Can't represent format {} as exr", (int) fmt);
			return WriteError::unsupportedFormat;
	}

	int pixTypes[4] {pixType, pixType, pixType, pixType};

	EXRHeader header {};
	header.channels = channels;
	header.num_channels = nc;
	header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;
	header.pixel_types = pixTypes;
	header.requested_pixel_types = pixTypes;

	auto pixelSize = vpp::formatSize(fmt);
	auto byteSize = width * height * pixelSize;

	auto data = provider.read();
	if(data.size() != byteSize) {
		dlg_warn("writeExr: expected {} bytes from provider, got {}",
			byteSize, data.size());
		return WriteError::readError;
	}

	// de-interlace
	auto deint = std::make_unique<std::byte[]>(byteSize);
	unsigned char* chanptrs[4];

	auto planeSize = width * height * chanSize;
	for(auto c = 0u; c < nc; ++c) {
		auto base = deint.get() + c * planeSize;
		for(auto y = 0u; y < height; ++y) {
			for(auto x = 0u; x < width; ++x) {
				auto address = y * width + x;
				auto src = data.data() + address * pixelSize + c * chanSize;
				auto dst = base + address * chanSize;
				std::memcpy(dst, src, chanSize);
			}
		}

		// channel order is reversed, see the switch above
		chanptrs[nc - c - 1] = reinterpret_cast<unsigned char*>(base);
	}

	EXRImage img {};
	img.width = width;
	img.height = height;
	img.images = chanptrs;
	img.num_channels = nc;

	const char* err {};
	auto res = SaveEXRImageToFile(&img, &header, path.c_str(), &err);
	if(res != TINYEXR_SUCCESS) {
		dlg_debug("SaveEXRImagetofile: {} ({})", err ? err : "-", res);
		FreeEXRErrorMessage(err);
		switch(res) {
			case TINYEXR_ERROR_CANT_OPEN_FILE:
				return WriteError::cantOpen;
			case TINYEXR_ERROR_CANT_WRITE_FILE:
				return WriteError::cantWrite;
			case TINYEXR_ERROR_UNSUPPORTED_FORMAT:
				return WriteError::unsupportedFormat;
			default:
				return WriteError::internal;
		}
	}

	return WriteError::none;
}

} // namespace tkn
