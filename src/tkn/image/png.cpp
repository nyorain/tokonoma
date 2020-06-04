#include <tkn/image.hpp>
#include <tkn/types.hpp>
#include <tkn/stream.hpp>

#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <nytl/span.hpp>
#include <dlg/dlg.hpp>
#include <dlg/dlg.hpp>

#include <png.h>
#include <csetjmp>
#include <cstdio>
#include <vector>

namespace tkn {

class PngReader : public ImageProvider {
public:
	std::unique_ptr<Stream> stream_ {};
	nytl::Vec2ui size_;
	png_infop pngInfo_ {};
	png_structp png_ {};
	mutable std::vector<std::byte> tmpData_ {};

public:
	~PngReader() {
		if(png_) {
			::png_destroy_read_struct(&png_, &pngInfo_, nullptr);
		}
	}

	nytl::Vec3ui size() const noexcept override { return {size_.x, size_.y, 1u}; }
	vk::Format format() const noexcept override { return vk::Format::r8g8b8a8Srgb; }

	u64 read(nytl::Span<std::byte> data, unsigned mip, unsigned layer) const override {
		dlg_assert(mip == 0);
		dlg_assert(layer == 0);

		auto byteSize = size_.x * size_.y * vpp::formatSize(format());
		dlg_assert(data.size() >= byteSize);

		auto rows = std::make_unique<png_bytep[]>(size_.y);
		if(::setjmp(png_jmpbuf(png_))) {
			throw std::runtime_error("setjmp(png_jmpbuf) failed");
		}

		auto rowSize = png_get_rowbytes(png_, pngInfo_);
		dlg_assert(rowSize == size_.x * vpp::formatSize(format()));

		auto ptr = reinterpret_cast<unsigned char*>(data.data());
		for(auto y = 0u; y < size_.y; ++y) {
			rows[y] = ptr + rowSize * y;
		}

		png_read_image(png_, rows.get());
		return byteSize;
	}

	nytl::Span<const std::byte> read(unsigned mip, unsigned layer) const override {
		auto byteSize = size_.x * size_.y * vpp::formatSize(format());
		tmpData_.resize(byteSize);
		auto res = read(tmpData_, mip, layer);
		dlg_assert(res == byteSize);
		return tmpData_;
	}
};

void readPngDataFromStream(png_structp png_ptr, png_bytep outBytes,
		png_size_t byteCountToRead) {
	png_voidp io_ptr = png_get_io_ptr(png_ptr);
	dlg_assert(io_ptr);

	Stream& stream = *(Stream*) io_ptr;
	auto res = stream.readPartial((std::byte*) outBytes, byteCountToRead);
	dlg_assert(res == i64(byteCountToRead));
}

ReadError readPng(std::unique_ptr<Stream>&& stream, PngReader& reader) {
	unsigned char sig[8];
	if(!stream->readPartial(sig)) {
		return ReadError::unexpectedEnd;
	}

	if(::png_sig_cmp(sig, 0, sizeof(sig))) {
    	return ReadError::invalidType;
  	}

	reader.png_ = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if(!reader.png_) {
    	return ReadError::invalidType;
	}

	reader.pngInfo_ = png_create_info_struct(reader.png_);
	if(!reader.pngInfo_) {
    	return ReadError::invalidType;
	}

	if(::setjmp(png_jmpbuf(reader.png_))) {
		return ReadError::internal;
	}

	png_set_read_fn(reader.png_, stream.get(), readPngDataFromStream);
	png_set_sig_bytes(reader.png_, sizeof(sig));
	png_read_info(reader.png_, reader.pngInfo_);

	// always read rgba8
	reader.size_.x = png_get_image_width(reader.png_, reader.pngInfo_);
	reader.size_.y = png_get_image_height(reader.png_, reader.pngInfo_);
	auto color_type = png_get_color_type(reader.png_, reader.pngInfo_);
	auto bit_depth  = png_get_bit_depth(reader.png_, reader.pngInfo_);

	if(bit_depth == 16) {
		png_set_strip_16(reader.png_);
	}

	if(color_type == PNG_COLOR_TYPE_PALETTE) {
		png_set_palette_to_rgb(reader.png_);
	}

	if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
		png_set_expand_gray_1_2_4_to_8(reader.png_);
	}

	if(png_get_valid(reader.png_, reader.pngInfo_, PNG_INFO_tRNS)) {
		png_set_tRNS_to_alpha(reader.png_);
	}

	if(color_type == PNG_COLOR_TYPE_RGB ||
			color_type == PNG_COLOR_TYPE_GRAY ||
			color_type == PNG_COLOR_TYPE_PALETTE) {
		png_set_filler(reader.png_, 0xFF, PNG_FILLER_AFTER);
	}

	if(color_type == PNG_COLOR_TYPE_GRAY ||
			color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(reader.png_);
	}

	png_read_update_info(reader.png_, reader.pngInfo_);
	reader.stream_ = std::move(stream);
	return ReadError::none;
}

ReadError readPng(std::unique_ptr<Stream>&& stream,
		std::unique_ptr<ImageProvider>& ret) {
	auto reader = std::make_unique<PngReader>();
	auto err = readPng(std::move(stream), *reader);
	if(err == ReadError::none) {
		ret = std::move(reader);
	}

	return err;
}

WriteError writePng(nytl::StringParam path, ImageProvider& img) {
	auto file = File(path, "wb");
	if(!file) {
		dlg_debug("fopen: {}", std::strerror(errno));
		return WriteError::cantOpen;
	}

	auto png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
		nullptr, nullptr, nullptr);
	if(!png) {
		dlg_error("png_create_write_struct returned null");
		return WriteError::internal;
	}

	auto info = png_create_info_struct(png);
	if(!info) {
		dlg_error("png_create_info_struct returned null");
		return WriteError::internal;
	}

	if(::setjmp(png_jmpbuf(png))) {
		dlg_error("png error (jmpbuf)");
		return WriteError::internal;
	}

	png_init_io(png, file);
	auto type = 0;
	auto comps = 0;
	if(img.format() == vk::Format::r8g8b8Unorm) {
		type = PNG_COLOR_TYPE_RGB;
		comps = 3;
	} else if(img.format() == vk::Format::r8g8b8a8Unorm) {
		type = PNG_COLOR_TYPE_RGBA;
		comps = 4;
	} else {
		dlg_error("Can only write rgb or rgba images as png");
		return WriteError::invalidFormat;
	}

	png_set_IHDR(png, info, img.size().x, img.size().y,
		8, type, PNG_INTERLACE_NONE,
    	PNG_COMPRESSION_TYPE_DEFAULT,
    	PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);

	auto s = img.size();
	auto data = img.read();
	if(data.size() != s.x * s.y * comps) {
		dlg_error("Invalid image data size. Expected {}, got {}",
			s.x * s.y * comps, data.size());
		return WriteError::readError;
	}

	auto rows = std::make_unique<png_bytep[]>(s.y);
	for(auto y = 0u; y < img.size().y; ++y) {
		auto off = y * s.x * comps;

		// ugh, the libpng api is terrible. This param should be const
		auto ptr = reinterpret_cast<const unsigned char*>(data.data() + off);
		rows[y] = const_cast<unsigned char*>(ptr);
	}

	png_write_image(png, rows.get());
	png_write_end(png, nullptr);
	png_destroy_write_struct(&png, &info);
	return WriteError::none;
}

} // namespace tkn
