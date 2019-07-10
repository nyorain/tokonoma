#include <tkn/image.hpp>
#include <tkn/types.hpp>

#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <nytl/span.hpp>
#include <dlg/dlg.hpp>

#include <turbojpeg.h>

#include <cstdio>
#include <vector>
#ifdef DOI_LINUX
	#include <sys/mman.h>
	#include <unistd.h>
	#include <fcntl.h>
#endif

namespace tkn {

// TODO: unix only atm. Really efficient though, using mmap.
// probably best to implement C-based alternative that is used on
// non-unix (or non-linux; not sure how cross platform mmap is) platforms.
// That should probably first manually copy the whole file in to a large buffer.
class JpegReader : public ImageProvider {
public:
	u64 fileLength_ {};
	unsigned char* data_ {};
	nytl::Vec2ui size_;
	tjhandle jpeg_ {};
	std::vector<std::byte> tmpData_;
#ifdef DOI_LINUX
	int fd_ {};
#else
	std::FILE* file_ {};
#endif


public:
	~JpegReader() {
		if(jpeg_) {
			::tjDestroy(jpeg_);
		}
#ifdef DOI_LINUX
		if(data_) {
			::munmap(data_, fileLength_);
		}
		if(fd_ > 0) {
			::close(fd_);
		}
#else
		if(data_) {
			std::free(data_);
		}
		if(file_) {
			std::fclose(file_);
		}
#endif
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

ReadError readJpeg(nytl::StringParam filename, JpegReader& reader) {
#ifdef DOI_LINUX
	auto fd = ::open(filename.c_str(), O_RDONLY);
	if(fd < 0) {
		return ReadError::cantOpen;
	}
	reader.fd_ = fd;

	auto length = ::lseek(reader.fd_, 0, SEEK_END);
	if(length < 0) {
		return ReadError::internal;
	}
	reader.fileLength_ = length;

	auto data = ::mmap(NULL, reader.fileLength_, PROT_READ, MAP_PRIVATE,
		reader.fd_, 0);
	if(data == MAP_FAILED || !data) {
		return ReadError::internal;
	}
	reader.data_ = static_cast<unsigned char*>(data); // const
#else
	reader.file_ = std::fopen(filename.c_str(), "rb");
	if(!reader.file_) {
		return ReadError::cantOpen;
	}

	// read data
	auto length = std::fseek(reader.file_, 0, SEEK_END);
	if(length < 0) {
		return ReadError::internal;
	}
	reader.fileLength_ = length;
	reader.data_ = (unsigned char*) std::malloc(reader.fileLength_);
	if(!reader.data_) {
		return ReadError::internal;
	}

	std::rewind(reader.file_);
	auto ret = std::fread(reader.data_, 1, reader.fileLength_, reader.file_);
	if(ret != reader.fileLength_) {
		dlg_warn("fread failed: {}", ret);
		return ReadError::internal;
	}
#endif

	reader.jpeg_ = ::tjInitDecompress();
	if(!reader.jpeg_) {
		dlg_warn("Can't initialize jpeg decompressor ('{}')", filename);
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

} // namespace tkn
