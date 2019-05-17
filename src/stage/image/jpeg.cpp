#include <stage/image.hpp>
#include <stage/types.hpp>

#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <nytl/span.hpp>
#include <dlg/dlg.hpp>

#include <turbojpeg.h>

#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <vector>

using namespace doi::types;

namespace doi {

// TODO: unix only atm. Really efficient though, using mmap.
// probably best to implement C-based alternative that is used on
// non-unix (or non-linux; not sure how cross platform mmap is) platforms.
// That should probably first manually copy the whole file in to a large buffer.
class JpegReader : public ImageProvider {
public:
	int fd_ {};
	u64 fileLength_ {};
	unsigned char* mapped_ {};
	nytl::Vec2ui size_;
	tjhandle jpeg_ {};
	std::vector<std::byte> tmpData_;

public:
	~JpegReader() {
		if(jpeg_) {
			::tjDestroy(jpeg_);
		}
		if(mapped_) {
			::munmap(mapped_, fileLength_);
		}
		if(fd_ > 0) {
			::close(fd_);
		}
	}

	vk::Format format() const override { return vk::Format::r8g8b8a8Srgb; }
	nytl::Vec2ui size() const override { return size_; }

	bool read(nytl::Span<std::byte> data, unsigned face,
			unsigned mip, unsigned layer) override {
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

	nytl::Span<std::byte> read(unsigned face, unsigned mip,
			unsigned layer) override {
		tmpData_.resize(size_.x * size_.y * vpp::formatSize(format()));
		if(read(tmpData_, face, mip, layer)) {
			return tmpData_;
		}

		return {};
	}
};

ReadError readJpeg(nytl::StringParam filename, JpegReader& reader) {
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
	reader.mapped_ = static_cast<unsigned char*>(data); // const

	reader.jpeg_ = ::tjInitDecompress();
	if(!reader.jpeg_) {
		dlg_warn("Can't initialize jpeg decompressor ('{}')", filename);
		return ReadError::internal;
	}

	int width, height;
	int res = ::tjDecompressHeader(reader.jpeg_, reader.mapped_,
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

} // namespace doi
