#define _POSIX_C_SOURCE 200809L

#include <tkn/config.hpp>
#include <tkn/image.hpp>
#include <tkn/stream.hpp>
#include <tkn/types.hpp>
#include <tkn/config.hpp>

#include <vkpp/enums.hpp>
#include <vpp/formats.hpp>
#include <nytl/span.hpp>
#include <nytl/scope.hpp>
#include <dlg/dlg.hpp>

#include <turbojpeg.h>

#include <cstdio>
#include <vector>

namespace tkn {

class JpegReader : public ImageProvider {
public:
	nytl::Vec2ui size_;
	tjhandle jpeg_ {};
	StreamMemoryMap mmap_;
	mutable std::vector<std::byte> tmpData_;

public:
	~JpegReader() {
		if(jpeg_) {
			::tjDestroy(jpeg_);
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

		auto src = reinterpret_cast<const unsigned char*>(mmap_.data());
		auto dst = reinterpret_cast<unsigned char*>(data.data());
		auto res = ::tjDecompress2(jpeg_, src, mmap_.size(),
			dst, size_.x, 0, size_.y, TJPF_RGBA, TJFLAG_FASTDCT);
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

ReadError loadJpeg(std::unique_ptr<Stream>&& stream, JpegReader& reader) {
	try {
		reader.mmap_ = StreamMemoryMap(std::move(stream));
	} catch(const std::exception& err) {
		dlg_error("Mapping/reading jpeg file into memory failed: {}", err.what());
		return ReadError::internal;
	}

	// Somewhat hacky: when reading fails, we don't take ownership of stream.
	// But StreamMemoryMap already took the stream. When we return unsuccesfully,
	// we have to return ownership. On success (see the end of the function),
	// we simply unset this guard.
	auto returnGuard = nytl::ScopeGuard([&]{
		stream = reader.mmap_.release();
	});

	reader.jpeg_ = ::tjInitDecompress();
	if(!reader.jpeg_) {
		dlg_warn("Can't initialize jpeg decompressor");
		return ReadError::internal;
	}

	int width, height;

	auto data = reinterpret_cast<const unsigned char*>(reader.mmap_.data());
	int subsamp, colorspace;
	int res = ::tjDecompressHeader3(reader.jpeg_, data, reader.mmap_.size(),
		&width, &height, &subsamp, &colorspace);
	if(res) {
		// in this case, it's propbably just no jpeg
		return ReadError::invalidType;
	}

	reader.size_.x = width;
	reader.size_.y = height;
	returnGuard.unset();
	return ReadError::none;
}

ReadError loadJpeg(std::unique_ptr<Stream>&& stream, std::unique_ptr<ImageProvider>& ret) {
	auto reader = std::make_unique<JpegReader>();
	auto err = loadJpeg(std::move(stream), *reader);
	if(err == ReadError::none) {
		ret = std::move(reader);
	}

	return err;
}

} // namespace tkn
