#define _POSIX_C_SOURCE 200809L

#include <tkn/config.hpp>
#include <tkn/image.hpp>
#include <tkn/stream.hpp>
#include <tkn/types.hpp>

#include <vkpp/enums.hpp>
#include <vpp/formats.hpp>
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
	std::unique_ptr<Stream> stream_;
	bool mapped_ {}; // whether data_ was mapped
	mutable std::vector<std::byte> tmpData_;

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

	vk::Format format() const noexcept override { return vk::Format::r8g8b8a8Srgb; }
	nytl::Vec3ui size() const noexcept override { return {size_.x, size_.y, 1u}; }

	u64 read(nytl::Span<std::byte> data, unsigned mip,
			unsigned layer) const override {
		dlg_assert(mip == 0);
		dlg_assert(layer == 0);

		auto byteSize = size_.x * size_.y * vpp::formatSize(format());
		dlg_assert(data.size() >= byteSize);

		auto ptr = reinterpret_cast<unsigned char*>(data.data());
		auto res = ::tjDecompress2(jpeg_, data_, fileLength_,
			ptr, size_.x, 0, size_.y, TJPF_RGBA, TJFLAG_FASTDCT);
		if(res != 0u) {
			auto msg = dlg::format("tjDecompress2: {}", res);
			dlg_warn(msg);
			throw std::runtime_error(msg);
		}

		return byteSize;
	}

	nytl::Span<const std::byte> read(unsigned mip, unsigned layer) const override {
		tmpData_.resize(size_.x * size_.y * vpp::formatSize(format()));
		auto res = read(tmpData_, mip, layer);
		dlg_assert(res == tmpData_.size());
		return tmpData_;
	}
};

ReadError readJpeg(std::unique_ptr<Stream>&& stream, JpegReader& reader) {
	int fd = -1;
#ifdef TKN_LINUX
	if(auto fstream = dynamic_cast<tkn::FileStream*>(stream.get()); fstream) {
		fd = fileno(fstream->file());
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
	}
#endif // TKN_LINUX

	if(fd < 0) {
		stream->seek(0, Stream::SeekOrigin::end);
		reader.fileLength_ = stream->address();

		// will be freed when reader is destroyed.
		reader.data_ = (unsigned char*) std::malloc(reader.fileLength_);
		if(!reader.data_) {
			dlg_error("malloc: {}", std::strerror(errno));
			return ReadError::internal;
		}

		stream->seek(0, Stream::SeekOrigin::set);

		errno = 0;
		auto ptr = reinterpret_cast<std::byte*>(reader.data_);
		auto res = stream->readPartial(ptr, reader.fileLength_);
		if(res < 0) {
			dlg_warn("stream read failed: errno: {} ({})", res, std::strerror(errno));
			return ReadError::internal;
		} else if(res < i64(reader.fileLength_)) {
			dlg_warn("Could not read complete jpeg file");
			return ReadError::unexpectedEnd;
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
	reader.stream_ = std::move(stream);
	return ReadError::none;
}

ReadError readJpeg(std::unique_ptr<Stream>&& stream, std::unique_ptr<ImageProvider>& ret) {
	auto reader = std::make_unique<JpegReader>();
	auto err = readJpeg(std::move(stream), *reader);
	if(err == ReadError::none) {
		ret = std::move(reader);
	}

	return err;
}

} // namespace tkn
