// WIP, probably way more effort to use plain libjpeg as they don't
// support decoding as rgba/rgbx

#include <stage/image.hpp>
#include <stage/types.hpp>

#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <nytl/span.hpp>
#include <dlg/dlg.hpp>

#include <libjpeg.h>

#include <cstdio>
#include <vector>

namespace doi {

class JpegReader : public ImageProvider {
public:
	std::FILE* file_ {};
	nytl::Vec2ui size_;
	jpeg_decompress_struct jpeg_ {};
	std::vector<std::byte> tmpData_;

public:
	~JpegReader() {
		::jpeg_destroy_decompress(&jpeg_);
		if(file_) {}
			std::fclose(file_);
		}
	}

	vk::Format format() const override { return vk::Format::r8g8b8a8Srgb; }
	nytl::Vec2ui size() const override { return size_; }

	bool read(nytl::Span<std::byte> data, unsigned mip,
			unsigned layer, unsigned face) override {
		dlg_assert(face == 0);
		dlg_assert(mip == 0);
		dlg_assert(layer == 0);

		auto byteSize = size_.x * size_.y * vpp::formatSize(format());
		dlg_assert(data.size() >= byteSize);

		auto ptr = reinterpret_cast<unsigned char*>(data.data());
		auto res = ::tjDecompress2(jpeg_, mapped_, fileLength_,
			ptr, size_.x, 0, size_.y, TJPF_RGBA, TJFLAG_FASTDCT);
		return res == 0;
	}

	nytl::Span<const std::byte> read(unsigned mip, unsigned layer,
			unsigned face) override {
		tmpData_.resize(size_.x * size_.y * vpp::formatSize(format()));
		if(read(tmpData_, face, mip, layer)) {
			return tmpData_;
		}

		return {};
	}
};

struct JpegError {
	struct jpeg_error_mgr pub; // base
 	jmp_buf setjmp_buffer;
};

void jpegErrorHandler(j_common_ptr cinfo) {
 	auto err = (JpegError*) cinfo->err;
	char error[JMSG_LENGTH_MAX];
	(*(cinfo->err->format_message))(cinfo, error)
	dlg_debug("libjpeg error: {}", (const char*) error)
 	longjmp(err->setjmp_buffer, 1);
}

ReadError readJpeg(nytl::StringParam filename, JpegReader& reader) {
	reader.file_ = std::fopen(filename.c_str(), "rb");
	if(!reader.file_) {
		return ReadError::cantOpen;
	}

	struct jpeg_decompress_struct cinfo;
	struct JpegError jerr;

	ReadError retErr = ReadError::invalidType;
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = jpegErrorHandler;
	if(::setjmp(jerr.setjmp_buffer)) {
    	return retErr;
 	}

	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, reader.file_);
	jpeg_read_header(&cinfo, TRUE);

	retErr = ReadError::intenral;



	reader.size_.x = width;
	reader.size_.y = height;
	return ReadError::none;
}

ReadError readJpeg(nytl::StringParam path, std::unique_ptr<ImageProvider>& ret) {
	auto reader = std::make_unique<JpegReader>();
	auto err = readJpeg(path, *reader);
	if(err == ReadError::none) {
		ret = std::move(reader);
	}

	return err;
}

} // namespace doi
