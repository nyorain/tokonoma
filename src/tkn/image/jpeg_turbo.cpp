#define _POSIX_C_SOURCE 200809L

#include <tkn/image.hpp>
#include <tkn/types.hpp>

#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <nytl/span.hpp>
#include <dlg/dlg.hpp>

#include <turbojpeg.h>

#include <cstdio>
#include <vector>

// on linux we use a mmap optimization
#ifdef TKN_LINUX
	#include <sys/mman.h>
	#include <unistd.h>
	#include <fcntl.h>
#endif

namespace tkn {

class JpegReader : public ImageProvider {
public:
	u64 fileLength_ {};
	unsigned char* data_ {};
	nytl::Vec2ui size_;
	tjhandle jpeg_ {};
	std::vector<std::byte> tmpData_;
	File file_;
	bool mapped_ {}; // whether data_ was mapped

public:
	~JpegReader() {
		if(jpeg_) {
			::tjDestroy(jpeg_);
		}

#ifdef TKN_LINUX
		if(mapped_ && data_) {
			::munmap(data_, fileLength_);
		}
#endif

		if(!mapped_) {
			std::free(data_);
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
		auto res = ::tjDecompress2(jpeg_, data_, fileLength_,
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

ReadError readJpeg(File&& file, JpegReader& reader) {
	int fd = -1;
#ifdef TKN_LINUX
	fd = fileno(file);
	if(fd < 0) {
		// this may happen for custom FILE objects.
		// It's not an error, we just fall back to the default non-mmap
		// implementation
		dlg_debug("fileno: {} ", std::strerror(errno));
	} else {
		auto length = ::lseek(fd, 0, SEEK_END);
		if(length < 0) {
			return ReadError::internal;
		}
		reader.fileLength_ = length;

		auto data = ::mmap(NULL, reader.fileLength_, PROT_READ, MAP_PRIVATE,
			fd, 0);
		if(data == MAP_FAILED || !data) {
			dlg_error("mmap failed: {}", std::strerror(errno));
			return ReadError::internal;
		}
		reader.mapped_ = true;
		reader.data_ = static_cast<unsigned char*>(data); // const
	}
#endif // TKN_LINUX

	if(fd < 0) {
		auto length = std::fseek(file, 0, SEEK_END);
		if(length < 0) {
			dlg_error("fseek: {}", std::strerror(errno));
			return ReadError::internal;
		}
		reader.fileLength_ = length;
		reader.data_ = (unsigned char*) std::malloc(reader.fileLength_);
		if(!reader.data_) {
			dlg_error("malloc: {}", std::strerror(errno));
			return ReadError::internal;
		}

		std::rewind(file);

		errno = 0;
		auto ret = std::fread(reader.data_, 1, reader.fileLength_, file);
		if(ret != reader.fileLength_) {
			dlg_warn("fread failed: {} ({})", ret, std::strerror(errno));
			return ReadError::internal;
		}
	}

	reader.jpeg_ = ::tjInitDecompress();
	if(!reader.jpeg_) {
		dlg_warn("Can't initialize jpeg decompressor");
		return ReadError::internal;
	}

	int width, height;
	int res = ::tjDecompressHeader(reader.jpeg_, reader.data_,
		reader.fileLength_, &width, &height);
	if(res) {
		// in this case, it's propbably just no jpeg
		return ReadError::invalidType;
	}

	reader.size_.x = width;
	reader.size_.y = height;
	reader.file_ = std::move(file);
	return ReadError::none;
}

ReadError readJpeg(File&& file, std::unique_ptr<ImageProvider>& ret) {
	std::rewind(file);
	auto reader = std::make_unique<JpegReader>();
	auto err = readJpeg(std::move(file), *reader);
	if(err == ReadError::none) {
		ret = std::move(reader);
	}

	return err;
}

ReadError readJpeg(nytl::StringParam path, std::unique_ptr<ImageProvider>& ret) {
	auto file = File(path, "rb");
	if(!file) {
		dlg_debug("fopen: {}", std::strerror(errno));
		return ReadError::cantOpen;
	}

	return readJpeg(std::move(file), ret);
}

} // namespace tkn
