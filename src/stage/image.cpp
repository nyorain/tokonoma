#include <stage/image.hpp>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>

#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <csetjmp>

#include <turbojpeg.h>
#include <png.h>

namespace doi {

// TODO: unix only atm. Really efficient though, using mmap.
// probably best to implement C-based alternative that is used on
// non-unix (or non-linux; not sure how cross platform mmap is) platforms
Error loadJpeg(nytl::StringParam filename, Image& img) {
	auto fd = ::open(filename.c_str(), O_RDONLY);
	if(fd < 0) {
		// dlg_warn("Image file doesn't exist: '{}'", filename);
		return Error::invalidPath;
	}

	auto fdGuard = nytl::ScopeGuard([&]{ ::close(fd); });
	auto len = ::lseek(fd, 0, SEEK_END);
	if(len < 0) {
		// dlg_warn("Can't seek on image '{}'", filename);
		return Error::invalidPath;
	}

	auto data = ::mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if(data == MAP_FAILED || !data) {
		// dlg_warn("Can't mmap image '{}'", filename);
		return Error::invalidPath;
	}
	auto mmapGuard = nytl::ScopeGuard([&]{ ::munmap(data, len); });
	auto cdata = static_cast<unsigned char*>(data); // const

	auto jpeg = ::tjInitDecompress();
	if(!jpeg) {
		// dlg_warn("Can't initialize jpeg decompressor ('{}')", filename);
		return Error::internal;
	}
	auto jpegGuard = nytl::ScopeGuard([&]{ ::tjDestroy(jpeg); });

	int width, height;
	int res = ::tjDecompressHeader(jpeg, cdata, len, &width, &height);
	if(res) {
		// dlg_warn("Invalid jpeg header (error {}), probably not jpeg: '{}'",
			// res, filename);
		return Error::invalidType;
	}

	auto buffer = std::make_unique<std::byte[]>(width * height * 4);
	auto ptr = reinterpret_cast<unsigned char*>(buffer.get());
	res = ::tjDecompress2(jpeg, cdata, len, ptr, width, 0, height,
		TJPF_RGBA, TJFLAG_FASTDCT);
	if(res) {
		// dlg_warn("Can't decompress jpeg (error {}): '{}'", res, filename);
		return Error::internal;
	}

	img.width = width;
	img.height = height;
	img.data = std::move(buffer);
	return Error::none;
}

Error loadPng(nytl::StringParam filename, Image& img) {
	auto file = ::fopen(filename.c_str(), "rb");
	if(!file) {
		return Error::invalidPath;
	}
	auto fileGuard = nytl::ScopeGuard([&]{ ::fclose(file); });

	unsigned char sig[8];
	::fread(sig, 1, sizeof(sig), file);
	if(::png_sig_cmp(sig, 0, sizeof(sig))) {
    	return Error::invalidType;
  	}

	png_infop info = nullptr;
	auto png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
		nullptr, nullptr);
	if(!png) {
    	return Error::invalidType;
	}
	auto pngGuard = nytl::ScopeGuard([&]{
		::png_destroy_read_struct(&png, &info, nullptr);
	});

	info = png_create_info_struct(png);
	if(!info) {
    	return Error::invalidType;
	}

	if(::setjmp(png_jmpbuf(png))) {
		return Error::invalidType;
	}

	png_init_io(png, file);
	png_set_sig_bytes(png, sizeof(sig));
	png_read_info(png, info);

	// always read rgba8
	auto width = png_get_image_width(png, info);
	auto height = png_get_image_height(png, info);
	auto color_type = png_get_color_type(png, info);
	auto bit_depth  = png_get_bit_depth(png, info);

	if(bit_depth == 16) {
		png_set_strip_16(png);
	}

	if(color_type == PNG_COLOR_TYPE_PALETTE) {
		png_set_palette_to_rgb(png);
	}

	if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
		png_set_expand_gray_1_2_4_to_8(png);
	}

	if(png_get_valid(png, info, PNG_INFO_tRNS)) {
		png_set_tRNS_to_alpha(png);
	}

	if(color_type == PNG_COLOR_TYPE_RGB ||
			color_type == PNG_COLOR_TYPE_GRAY ||
			color_type == PNG_COLOR_TYPE_PALETTE) {
		png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
	}

	if(color_type == PNG_COLOR_TYPE_GRAY ||
			color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
		png_set_gray_to_rgb(png);
	}

	png_read_update_info(png, info);

	std::unique_ptr<png_bytep[]> rows;
	std::unique_ptr<std::byte[]> buffer;
	if(::setjmp(png_jmpbuf(png))) {
		return Error::internal;
	}

	rows = std::make_unique<png_bytep[]>(height);
	auto rowSize = png_get_rowbytes(png, info);
	buffer = std::make_unique<std::byte[]>(height * rowSize);
	auto ptr = reinterpret_cast<unsigned char*>(buffer.get());
  	for(auto y = 0u; y < height; ++y) {
		rows[y] = ptr + rowSize * y;
	}

	png_read_image(png, rows.get());

	img.width = width;
	img.height = height;
	img.data = std::move(buffer);
	return Error::none;
}

Error load(nytl::StringParam filename, Image& img) {
	auto err = loadPng(filename, img);
	if(err == Error::none) {
		return err;
	}

	return loadJpeg(filename, img);
}

} // namespace doi
