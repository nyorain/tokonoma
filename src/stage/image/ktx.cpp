#include <stage/image.hpp>
#include <stage/types.hpp>
#include <stage/gl.hpp>
#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <nytl/scope.hpp>
#include <dlg/dlg.hpp>

// TODO: support for compressed formats and such

namespace doi {

constexpr struct FormatEntry {
	GLInternalFormat glFormat;
	u32 glPixelFormat;
	u32 glPixelType;
	vk::Format vkFormat;
} formatMap[] = {
	// 8bit
	{GL_R8, GL_RED, GL_UNSIGNED_BYTE, vk::Format::r8Unorm},
	{GL_RG8, GL_RG, GL_UNSIGNED_BYTE, vk::Format::r8g8Unorm},
	{GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, vk::Format::r8g8b8Unorm},
	{GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, vk::Format::r8g8b8a8Unorm},

	{GL_SR8, GL_RED, GL_UNSIGNED_BYTE, vk::Format::r8Srgb},
	{GL_SRGB8, GL_RGB, GL_UNSIGNED_BYTE, vk::Format::r8g8b8Srgb},
	{GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, vk::Format::r8g8b8a8Srgb},

	{GL_R8_SNORM, GL_RED, GL_BYTE, vk::Format::r8Snorm},
	{GL_RG8_SNORM, GL_RED, GL_BYTE, vk::Format::r8g8Snorm},
	{GL_RGB8_SNORM, GL_RED, GL_BYTE, vk::Format::r8g8b8Snorm},
	{GL_RGBA8_SNORM, GL_RED, GL_BYTE, vk::Format::r8g8b8a8Snorm},

	{GL_R8I, GL_RED_INTEGER, GL_BYTE, vk::Format::r8Sint},
	{GL_RG8I, GL_RG_INTEGER, GL_BYTE, vk::Format::r8g8Sint},
	{GL_RGB8I, GL_RGB_INTEGER, GL_BYTE, vk::Format::r8g8b8Sint},
	{GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE, vk::Format::r8g8b8a8Sint},

	{GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, vk::Format::r8Uint},
	{GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, vk::Format::r8g8Uint},
	{GL_RGB8UI, GL_RGB_INTEGER, GL_UNSIGNED_BYTE, vk::Format::r8g8b8Uint},
	{GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, vk::Format::r8g8b8a8Uint},

	// 16it
	{GL_R16, GL_RED, GL_UNSIGNED_SHORT, vk::Format::r16Unorm},
	{GL_RG16, GL_RG, GL_UNSIGNED_SHORT, vk::Format::r16g16Unorm},
	{GL_RGB16, GL_RGB, GL_UNSIGNED_SHORT, vk::Format::r16g16b16Unorm},
	{GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT, vk::Format::r16g16b16a16Unorm},

	{GL_R16F, GL_RED, GL_FLOAT, vk::Format::r16Sfloat},
	{GL_RG16F, GL_RG, GL_FLOAT, vk::Format::r16g16Sfloat},
	{GL_RGB16F, GL_RGB, GL_FLOAT, vk::Format::r16g16b16Sfloat},
	{GL_RGBA16F, GL_RGBA, GL_FLOAT, vk::Format::r16g16b16a16Sfloat},

	{GL_R16_SNORM, GL_RED, GL_SHORT, vk::Format::r16Snorm},
	{GL_RG16_SNORM, GL_RG, GL_SHORT, vk::Format::r16g16Snorm},
	{GL_RGB16_SNORM, GL_RGBA, GL_SHORT, vk::Format::r16g16b16Snorm},

	{GL_R16I, GL_RED_INTEGER, GL_SHORT, vk::Format::r16Sint},
	{GL_RG16I, GL_RG_INTEGER, GL_SHORT, vk::Format::r16g16Sint},
	{GL_RGB16I, GL_RGB_INTEGER, GL_SHORT, vk::Format::r16g16b16Sint},
	{GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT, vk::Format::r16g16b16a16Sint},

	{GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, vk::Format::r16Uint},
	{GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT, vk::Format::r16g16Uint},
	{GL_RGB16UI, GL_RGB_INTEGER, GL_UNSIGNED_SHORT, vk::Format::r16g16b16Uint},
	{GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, vk::Format::r16g16b16a16Uint},

	// 32bit
	{GL_R32F, GL_RED, GL_FLOAT, vk::Format::r32Sfloat},
	{GL_RG32F, GL_RG, GL_FLOAT, vk::Format::r32g32Sfloat},
	{GL_RGBA32F, GL_RGBA, GL_FLOAT, vk::Format::r32g32b32a32Sfloat},

	{GL_R32I, GL_RED_INTEGER, GL_INT, vk::Format::r32Sint},
	{GL_RG32I, GL_RG_INTEGER, GL_INT, vk::Format::r32g32Sint},
	{GL_RGB32I, GL_RGB_INTEGER, GL_INT, vk::Format::r32g32b32Sint},
	{GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, vk::Format::r32g32b32a32Sint},

	{GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, vk::Format::r32Uint},
	{GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, vk::Format::r32g32Uint},
	{GL_RGB32UI, GL_RGB_INTEGER, GL_UNSIGNED_INT, vk::Format::r32g32b32Uint},
	{GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, vk::Format::r32g32b32a32Uint},
};

vk::Format vulkanFromGLFormat(GLInternalFormat glFormat) {
	for(auto& entry : formatMap) {
		if(entry.glFormat == glFormat) {
			return entry.vkFormat;
		}
	}

	return vk::Format::undefined;
}

// throwing io functions
// throw for partial read/write as well
void tfread(void* buffer, std::size_t size, std::size_t count,
		std::FILE* stream) {
	auto res = std::fread(buffer, size, count, stream);
	if(res != size * count) {
		dlg_error("fread: {} ({})", res, std::strerror(errno));
		throw std::runtime_error("fread failed");
	}
}

void tfwrite(void* buffer, std::size_t size, std::size_t count,
		std::FILE* stream) {
	auto res = std::fwrite(buffer, size, count, stream);
	if(res != count) {
		dlg_error("fwrite: {} ({})", res, std::strerror(errno));
		throw std::runtime_error("fwrite failed");
	}
}

u32 tfseek(std::FILE* stream, long offset, int origin) {
	auto res = std::fseek(stream, offset, origin);
	if(res < 0) {
		dlg_error("fseek: {} ({})", res, std::strerror(errno));
		throw std::runtime_error("fseek failed");
	}

	return u32(res);
}

u32 tftell(std::FILE* stream) {
	auto res = std::ftell(stream);
	if(res < 0) {
		dlg_error("ftell: {} ({})", res, std::strerror(errno));
		throw std::runtime_error("ftell failed");
	}

	return u32(res);
}

// wip
class KtxReader : public ImageProvider {
public:
	vk::Format format_;
	nytl::Vec2ui size_;
	u32 mipLevels_;
	u32 faces_;
	u32 arrayElements_; // 0 for non array textures
	std::FILE* file_;
	u64 dataBegin_;
	std::vector<std::byte> tmpData_;

public:
	~KtxReader() {
		if(file_) {
			::fclose(file_);
		}
	}

	u64 faceSize(unsigned mip) const {
		auto w = std::max(size_.x >> mip, 1u);
		auto h = std::max(size_.y >> mip, 1u);
		return w * h * vpp::formatSize(format_);
	}

	nytl::Vec2ui size() const override { return size_; }
	vk::Format format() const override { return format_; }
	unsigned mipLevels() const override { return mipLevels_; }
	unsigned faces() const override { return faces_; }
	unsigned layers() const override {
		return std::max(arrayElements_, 1u);
	}

	nytl::Span<const std::byte> read(unsigned mip = 0, unsigned layer = 0,
			unsigned face = 0) override {
		tmpData_.resize(faceSize(mip));
		if(read(tmpData_, face, mip, layer)) {
			return tmpData_;
		}

		return {};
	}

	bool read(nytl::Span<std::byte> data, unsigned mip = 0,
			unsigned layer = 0, unsigned face = 0) override {
		errno = {};
		dlg_assert(face < faces());
		dlg_assert(mip < mipLevels());
		dlg_assert(layer < layers());

		auto address = dataBegin_;
		u32 imageSize;
		for(auto i = 0u; i < mip; ++i) {
			auto faceSize = this->faceSize(i);
			auto mipSize = layers() * faces_ * vpp::align(faceSize, 4u);
			auto expectedImageSize = faceSize;
			if(arrayElements_ == 0 && faces_ == 6) {
				// ktx special cubemap imageSize case
				expectedImageSize = faceSize;
			}

			// debug imageSize reading
			tfseek(file_, address, SEEK_SET);
			tfread(&imageSize, 1, sizeof(imageSize), file_);
			if(imageSize != expectedImageSize) {
				dlg_error("KtxReader: unexpected imageSize {}, expected {}",
					imageSize, expectedImageSize);
				return false;
			}

			// add mip padding, imageSize is without padding
			mipSize = vpp::align(mipSize, 4u);

			address += 4u; // imageSize u32
			address += mipSize;
		}

		auto byteSize = this->faceSize(mip);
		auto faceSize = vpp::align(byteSize, 4u);
		dlg_assert(u64(data.size()) >= byteSize);

		// debug imageSize reading
		auto mipSize = layers() * faces_ * faceSize;
		if(arrayElements_ == 0 && faces_ == 6) {
			// ktx special case: imageSize only size of one face
			mipSize = byteSize;
		}

		tfseek(file_, address, SEEK_SET);
		tfread(&imageSize, 1, sizeof(imageSize), file_);
		if(imageSize != mipSize) {
			dlg_error("KtxReader: unexpected imageSize {}, expected {}",
				imageSize, mipSize);
			return false;
		}

		address += 4u; // imageSize u32
		address += layer * faces_ * faceSize; // skip prev layers
		address += face * faceSize; // skip prev faces

		// go to calculated address and read data
		tfseek(file_, address, SEEK_SET);
		tfread(data.data(), 1, byteSize, file_);

		return true;
	}
};

static constexpr u32 ktxEndianess = 0x04030201;
static constexpr std::array<u8, 12> ktxIdentifier = {
	0xAB, 0x4B, 0x54, 0x58, 0x20, 0x31, 0x31, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

struct KtxHeader {
	u32 endianness;
	u32 glType;
	u32 glTypeSize;
	u32 glFormat;
	u32 glInternalFormat;
	u32 glBaseInternalFormat;
	u32 pixelWidth;
	u32 pixelHeight;
	u32 pixelDepth;
	u32 numberArrayElements;
	u32 numberFaces;
	u32 numberMipmapLevels;
	u32 bytesKeyValueData;
};

ReadError readKtx(nytl::StringParam path, KtxReader& reader) {
	auto file = std::fopen(path.c_str(), "rb");
	if(!file) {
		return ReadError::cantOpen;
	}

	std::array<u8, 12> identifier;
	if(std::fread(identifier.data(), 1, 12, file) < 12) {
		std::fclose(file);
		return ReadError::unexpectedEnd;
	}

	if(identifier != ktxIdentifier) {
		std::fclose(file);
		return ReadError::invalidType;
	}

	KtxHeader header;
	if(std::fread(&header, 1u, sizeof(KtxHeader), file) < sizeof(KtxHeader)) {
		std::fclose(file);
		return ReadError::unexpectedEnd;
	}

	if(header.endianness != ktxEndianess) {
		// In this case the file was written in non-native endianess
		// we could support it but that will be a lot of work
		// Just error out for now
		dlg_debug("KTX invalid endianess: {}{}", std::hex, header.endianness);
		::fclose(file);
		return ReadError::invalidEndianess;
	}

	if(header.pixelDepth > 1) {
		dlg_warn("KTX pixelDepth {} > 1 unsupported", header.pixelDepth);
		std::fclose(file);
		return ReadError::ktxDepth;
	}

	if(header.pixelWidth == 0) {
		dlg_debug("KTX pixelWidth == 0");
		std::fclose(file);
		return ReadError::empty;
	}

	if(header.glFormat == 0) {
		dlg_warn("KTX compressed format, not supported yet");
		std::fclose(file);
		return ReadError::ktxInvalidFormat;
	}

	reader.file_ = file;

	// numberArrayElements == 0 has a special meaning for the imageSize
	// field, so don't set it to 1 if zero. Will be done in Reader
	reader.arrayElements_ = header.numberArrayElements;
	reader.faces_ = std::max(header.numberFaces, 1u);

	// NOTE: when numberMipmapLevels is zero, ktx specifies the loader
	// should generate mipmaps. We could probably forward that information
	// somehow? but in the end i guess the application knows whether or
	// not it wants mipmaps, right?
	reader.mipLevels_ = std::max(header.numberMipmapLevels, 1u);
	reader.size_ = {header.pixelWidth, std::max(header.pixelHeight, 1u)};

	auto glFormat = GLInternalFormat(header.glInternalFormat);
	reader.format_ = vulkanFromGLFormat(glFormat);
	if(reader.format_ == vk::Format::undefined) {
		dlg_warn("unsupported ktx format: {}", glFormat);
		std::fclose(file);
		return ReadError::ktxInvalidFormat;
	}

	auto curr = std::ftell(file);
	if(curr <= 0) {
		dlg_warn("ftell: {} ({})", std::strerror(errno), curr);
		std::fclose(file);
		return ReadError::internal;
	}

	reader.dataBegin_ = curr + header.bytesKeyValueData;

	return ReadError::none;
}

ReadError readKtx(nytl::StringParam path, std::unique_ptr<ImageProvider>& ret) {
	auto reader = std::make_unique<KtxReader>();
	auto err = readKtx(path, *reader);
	if(err == ReadError::none) {
		ret = std::move(reader);
	}

	return err;
}

// save
WriteError writeKtx(nytl::StringParam path, ImageProvider& image) {
	auto file = ::fopen(path.c_str(), "wb");
	if(!file) {
		return WriteError::cantOpen;
	}

	auto fileGuard = nytl::ScopeGuard([&]{ ::fclose(file); });
#define write(data, size)\
	if(::fwrite(data, size, 1, file) < 1) { \
		dlg_debug("fwrite failed"); \
		return WriteError::cantWrite; \
	}

	write(ktxIdentifier.data(), ktxIdentifier.size());

	auto fmt = image.format();
	auto size = image.size();
	auto mips = std::max(image.mipLevels(), 1u);
	auto layers = std::max(image.layers(), 1u);
	auto faces = std::max(image.faces(), 1u);
	auto fmtSize = vpp::formatSize(fmt);

	KtxHeader header;
	header.endianness = ktxEndianess;
	header.bytesKeyValueData = 0u;
	header.pixelWidth = size.x;
	header.pixelHeight = size.y;
	header.pixelDepth = 0;
	header.numberFaces = faces;
	header.numberMipmapLevels = mips;
	header.numberArrayElements = layers > 1 ? layers : 0;
	header.glTypeSize = fmtSize;

	const FormatEntry* entry {};
	for(auto& ientry : formatMap) {
		if(ientry.vkFormat == fmt) {
			entry = &ientry;
			break;
		}
	}

	if(!entry) {
		return WriteError::invalidFormat;
	}

	header.glFormat = entry->glPixelFormat;
	header.glBaseInternalFormat = entry->glPixelFormat;
	header.glType = entry->glPixelType;
	header.glInternalFormat = entry->glFormat;

	write(&header, sizeof(header));
	u8 zeroBytes[4] = {0, 0, 0, 0};

	for(auto m = 0u; m < mips; ++m) {
		// image size
		nytl::Vec2ui msize;
		msize.x = std::max(size.x >> m, 1u);
		msize.y = std::max(size.y >> m, 1u);
		u32 faceSize = msize.x * msize.y * fmtSize;

		// ktx exception: for this condition imagesize should only
		// contain the size of *one face* instead of everything.
		if(header.numberArrayElements == 0 && faces == 6) {
			write(&faceSize, sizeof(faceSize));
		} else {
			u32 fullSize = vpp::align(faceSize, 4u) * layers * faces;
			write(&fullSize, sizeof(fullSize));
		}

		for(auto l = 0u; l < layers; ++l) {
			for(auto f = 0u; f < faces; ++f) {
				auto span = image.read(m, l, f);
				if(span.size() != faceSize) {
					dlg_debug("invalid ImageProvider read size: "
						"got {}, expected {}", span.size(), faceSize);
					return WriteError::readError;
				}

				write(span.data(), u64(span.size()));

				// padding, align to 4
				auto off = ::ftell(file);
				dlg_assert(off > 0);
				u32 padding = vpp::align(off, 4u) - off;
				if(padding > 0) {
					write(zeroBytes, padding);
				}
			}
		}

		// padding, align to 4
		auto off = ::ftell(file);
		dlg_assert(off > 0);
		u32 padding = vpp::align(off, 4u) - off;
		if(padding > 0) {
			write(zeroBytes, padding);
		}
	}

#undef write

	return WriteError::none;
}

} // namespace doi
