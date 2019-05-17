#include <stage/image.hpp>
#include <stage/types.hpp>

#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <nytl/span.hpp>
#include <dlg/dlg.hpp>
#include <dlg/dlg.hpp>

#include <png.h>
#include <csetjmp>
#include <cstdio>
#include <vector>

using namespace doi::types;

namespace doi {

class PngReader : public ImageProvider {
public:
	std::FILE* file_ {};
	nytl::Vec2ui size_;
	png_infop pngInfo_ {};
	png_structp png_ {};
	std::vector<std::byte> tmpData_ {};

public:
	~PngReader() {
		if(png_) {
			::png_destroy_read_struct(&png_, &pngInfo_, nullptr);
		}
		if(file_) {
			std::fclose(file_);
		}
	}

	nytl::Vec2ui size() const override { return size_; }
	vk::Format format() const override { return vk::Format::r8g8b8a8Srgb; }

	bool read(nytl::Span<std::byte> data, unsigned face,
			unsigned mip, unsigned layer) override {
		dlg_assert(face == 0);
		dlg_assert(mip == 0);
		dlg_assert(layer == 0);

		auto byteSize = size_.x * size_.y * vpp::formatSize(format());
		dlg_assert(data.size() >= byteSize);

		auto rows = std::make_unique<png_bytep[]>(size_.y);
		if(::setjmp(png_jmpbuf(png_))) {
			return false;
		}

		auto rowSize = png_get_rowbytes(png_, pngInfo_);
		dlg_assert(rowSize == size_.x * vpp::formatSize(format()));

		auto ptr = reinterpret_cast<unsigned char*>(data.data());
		for(auto y = 0u; y < size_.y; ++y) {
			rows[y] = ptr + rowSize * y;
		}

		png_read_image(png_, rows.get());
		return true;
	}

	nytl::Span<std::byte> read(unsigned face, unsigned mip,
			unsigned layer) override {
		tmpData_.resize(size_.x * size_.y * vpp::formatSize(format()));
		if(read(tmpData_, face, mip, layer)) {
			return tmpData_;
		}

		return {};
	}
};

ReadError readPng(nytl::StringParam path, PngReader& reader) {
	reader.file_ = ::fopen(path.c_str(), "rb");
	if(!reader.file_) {
		return ReadError::cantOpen;
	}

	unsigned char sig[8];
	::fread(sig, 1, sizeof(sig), reader.file_);
	if(::png_sig_cmp(sig, 0, sizeof(sig))) {
    	return ReadError::invalidType;
  	}

	reader.png_ = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
		nullptr, nullptr);
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

	png_init_io(reader.png_, reader.file_);
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
	return ReadError::none;
}

} // namespace doi
