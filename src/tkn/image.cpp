#include <tkn/image.hpp>
#include <tkn/types.hpp>
#include <tkn/stream.hpp>
#include <tkn/util.hpp>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/span.hpp>
#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <dlg/dlg.hpp>
#include <cstdio>

// Make stbi std::unique_ptr<std::byte[]> compatible.
// Needed since calling delete on a pointer allocated with malloc
// is undefined behavior.
namespace {
void* stbiRealloc(void* old, std::size_t newSize) {
	delete[] (std::byte*) old;
	return (void*) (new std::byte[newSize]);
}
} // anon namespace

#define STBI_FREE(p) (delete[] (std::byte*) p)
#define STBI_MALLOC(size) ((void*) new std::byte[size])
#define STBI_REALLOC(p, size) (stbiRealloc((void*) p, size))
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC // needed, otherwise we mess with other usages

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace tkn {

// ImageProvider api
std::unique_ptr<ImageProvider> readStb(std::unique_ptr<Stream>&& stream) {
	auto img = readImageStb(std::move(stream));
	if(!img.data) {
		return {};
	}

	return wrap(std::move(img));
}

std::unique_ptr<ImageProvider> read(std::unique_ptr<Stream>&& stream,
		std::string_view ext) {
	std::unique_ptr<ImageProvider> reader;
	bool triedpng = false;
	bool triedjpg = false;
	bool triedktx = false;
	bool triedstb = false;
	bool triedexr = false;

	// first try detecting the type based of suffix
	if(has_suffix(ext, ".png")) {
		triedpng = true;
		if(readPng(std::move(stream), reader) == ReadError::none) {
			return reader;
		}
	} else if(has_suffix(ext, ".jpg") || has_suffix(ext, ".jpeg")) {
		triedjpg = true;
		if(readJpeg(std::move(stream), reader) == ReadError::none) {
			return reader;
		}
	} else if(has_suffix(ext, ".ktx")) {
		triedktx = true;
		if(readKtx(std::move(stream), reader) == ReadError::none) {
			return reader;
		}
	} else if(has_suffix(ext, ".hdr")) {
		triedstb = true;
		reader = readStb(std::move(stream));
		if(reader) {
			return reader;
		}
	} else if(has_suffix(ext, ".exr")) {
		triedexr = true;
		if(readExr(std::move(stream), reader) == ReadError::none) {
			return reader;
		}
	}

	// then just try all remaining loaders
	stream->seek(0, Stream::SeekOrigin::set);
	if(!triedpng && readPng(std::move(stream), reader) == ReadError::none) {
		return reader;
	}

	stream->seek(0, Stream::SeekOrigin::set);
	if(!triedjpg && readJpeg(std::move(stream), reader) == ReadError::none) {
		return reader;
	}

	stream->seek(0, Stream::SeekOrigin::set);
	if(!triedktx && readKtx(std::move(stream), reader) == ReadError::none) {
		return reader;
	}

	stream->seek(0, Stream::SeekOrigin::set);
	if(!triedexr && readExr(std::move(stream), reader) == ReadError::none) {
		return reader;
	}

	stream->seek(0, Stream::SeekOrigin::set);
	if(!triedstb) {
		return readStb(std::move(stream));
	}

	return nullptr;
}

std::unique_ptr<ImageProvider> read(nytl::StringParam path) {
	auto file = File(path, "rb");
	if(!file) {
		dlg_debug("fopen: {}", std::strerror(errno));
		return {};
	}

	return read(std::make_unique<FileStream>(std::move(file)), path);
}

std::unique_ptr<ImageProvider> read(File&& file) {
	return read(std::make_unique<FileStream>(std::move(file)));
}

std::unique_ptr<ImageProvider> read(nytl::Span<const std::byte> data) {
	return read(std::make_unique<MemoryStream>(data));
}

// Image api
Image readImageStb(std::unique_ptr<Stream>&& stream) {
	constexpr auto channels = 4u; // TODO: make configurable
	int width, height, ch;
	std::byte* data;
	Image ret;

	auto& cb = streamStbiCallbacks();
	bool hdr = stbi_is_hdr_from_callbacks(&cb, stream.get());
	if(hdr) {
		auto fd = stbi_loadf_from_callbacks(&cb, stream.get(), &width, &height, &ch, channels);
		data = reinterpret_cast<std::byte*>(fd);
		ret.format = vk::Format::r32g32b32a32Sfloat;
	} else {
		auto cd = stbi_load_from_callbacks(&cb, stream.get(), &width, &height, &ch, channels);
		data = reinterpret_cast<std::byte*>(cd);
		ret.format = vk::Format::r8g8b8a8Unorm;
	}

	if(!data) {
		std::string err = "STB could not load image from file: ";
		err += stbi_failure_reason();
		dlg_warn("{}", err);
		return ret;
	}

	ret.data.reset(data);
	ret.size.x = width;
	ret.size.y = height;
	ret.size.z = 1u;
	stream = {}; // no longer needed, close it.
	return ret;
}

Image readImage(const ImageProvider& provider, unsigned mip, unsigned layer) {
	dlg_assertlm(dlg_level_debug, provider.layers() == 1,
		"readImage: discarding {} layers", provider.layers() - 1);
	dlg_assertlm(dlg_level_debug, provider.mipLevels() == 1,
		"readImage: discarding {} mip levels", provider.mipLevels() - 1);

	Image ret;
	ret.format = provider.format();

	auto size = provider.size();
	size.x = std::max(size.x >> mip, 1u);
	size.y = std::max(size.y >> mip, 1u);
	size.z = std::max(size.z >> mip, 1u);

	ret.size = size;
	auto byteSize = size.x * size.y * size.z * vpp::formatSize(ret.format);
	ret.data = std::make_unique<std::byte[]>(byteSize);
	auto res = provider.read({ret.data.get(), ret.data.get() + byteSize}, mip, layer);
	dlg_assert(res == byteSize);

	return ret;
}

Image readImage(std::unique_ptr<Stream>&& stream, unsigned mip, unsigned layer) {
	auto provider = read(std::move(stream));
	return provider ? readImage(*provider, mip, layer) : Image {};
}

class MemImageProvider : public ImageProvider {
public:
	struct MipLayerFace {
		std::unique_ptr<std::byte[]> owned;
		const std::byte* ref;
	};

	std::vector<MipLayerFace> data_;
	bool cubemap_ {};
	unsigned layers_;
	unsigned mips_;
	nytl::Vec3ui size_;
	vk::Format format_;

public:
	u64 faceSize(unsigned mip) const {
		auto w = std::max(size_.x >> mip, 1u);
		auto h = std::max(size_.y >> mip, 1u);
		auto d = std::max(size_.z >> mip, 1u);
		return w * h * d * vpp::formatSize(format_);
	}

	unsigned layers() const noexcept override { return layers_; }
	unsigned mipLevels() const noexcept override { return mips_; }
	nytl::Vec3ui size() const noexcept override { return size_; }
	vk::Format format() const noexcept override { return format_; }
	bool cubemap() const noexcept override { return cubemap_; }

	nytl::Span<const std::byte> read(unsigned mip = 0, unsigned layer = 0) const override {
		dlg_assert(mip < mipLevels() && layer < layers());
		auto id = mip * layers_ + layer;
		return {data_[id].ref, data_[id].ref + faceSize(mip)};
	}

	u64 read(nytl::Span<std::byte> data, unsigned mip = 0, unsigned layer = 0) const override {
		dlg_assert(mip < mipLevels() && layer < layers());
		auto id = mip * layers_ + layer;
		auto byteSize = faceSize(mip);
		dlg_assert(u64(data.size()) >= byteSize);
		std::memcpy(data.data(), data_[id].ref, byteSize);
		return byteSize;
	}
};

std::unique_ptr<ImageProvider> wrap(Image&& image) {
	dlg_assert(image.size.x >= 1 && image.size.y >= 1 && image.size.z >= 1);

	auto ret = std::make_unique<MemImageProvider>();
	ret->layers_ = ret->mips_ = 1u;
	ret->format_ = image.format;
	ret->size_ = image.size;
	ret->cubemap_ = false;
	auto& data = ret->data_.emplace_back();
	data.owned = std::move(image.data);
	data.ref = data.owned.get();
	return ret;
}

std::unique_ptr<ImageProvider> wrap(nytl::Vec3ui size, vk::Format format,
		nytl::Span<const std::byte> span) {
	dlg_assert(size.x >= 1 && size.y >= 1 && size.z >= 1);
	dlg_assert(span.size() >= size.x * size.y * vpp::formatSize(format));

	auto ret = std::make_unique<MemImageProvider>();
	ret->layers_ = ret->mips_ = 1u;
	ret->format_ = format;
	ret->size_ = size;
	ret->cubemap_ = false;
	auto& data = ret->data_.emplace_back();
	data.ref = span.data();
	return ret;
}

std::unique_ptr<ImageProvider> wrap(nytl::Vec3ui size, vk::Format format,
		unsigned mips, unsigned layers,
		nytl::Span<std::unique_ptr<std::byte[]>> data, bool cubemap) {
	dlg_assert(size.x >= 1 && size.y >= 1 && size.z >= 1);
	dlg_assert(mips >= 1);
	dlg_assert(layers >= 1);
	dlg_assert(data.size() == mips * layers);
	dlg_assert(!cubemap || layers % 6 == 0u);

	auto ret = std::make_unique<MemImageProvider>();
	ret->mips_ = mips;
	ret->layers_ = layers;
	ret->format_ = format;
	ret->size_ = size;
	ret->cubemap_ = cubemap;
	ret->data_.reserve(data.size());
	for(auto& d : data) {
		auto& rdi = ret->data_.emplace_back();
		rdi.owned = std::move(d);
		rdi.ref = rdi.owned.get();
	}

	return ret;
}

std::unique_ptr<ImageProvider> wrap(nytl::Vec3ui size, vk::Format format,
		unsigned mips, unsigned layers, std::unique_ptr<std::byte[]> data,
		bool cubemap) {
	dlg_assert(size.x >= 1 && size.y >= 1 && size.z >= 1);
	dlg_assert(mips >= 1);
	dlg_assert(layers >= 1);
	dlg_assert(!cubemap || layers % 6 == 0u);

	auto ret = std::make_unique<MemImageProvider>();
	ret->mips_ = mips;
	ret->layers_ = layers;
	ret->format_ = format;
	ret->size_ = size;
	ret->cubemap_ = cubemap;
	ret->data_.reserve(mips * layers);

	auto fmtSize = vpp::formatSize(format);
	for(auto m = 0u; m < mips; ++m) {
		for(auto l = 0u; l < layers; ++l) {
			auto& rdi = ret->data_.emplace_back();
			auto off = vpp::imageBufferOffset(fmtSize,
				{size.x, size.y, size.z}, layers, m, l);
			rdi.ref = data.get() + off;
		}
	}

	ret->data_[0].owned = std::move(data);
	return ret;
}

std::unique_ptr<ImageProvider> wrap(nytl::Vec3ui size, vk::Format format,
		unsigned mips, unsigned layers,
		nytl::Span<const std::byte* const> data, bool cubemap) {
	dlg_assert(data.size() == mips * layers);
	dlg_assert(!cubemap || layers % 6 == 0u);

	auto ret = std::make_unique<MemImageProvider>();
	ret->mips_ = mips;
	ret->layers_ = layers;
	ret->format_ = format;
	ret->size_ = size;
	ret->cubemap_ = cubemap;
	ret->data_.reserve(data.size());
	for(auto& d : data) {
		auto& rdi = ret->data_.emplace_back();
		rdi.ref = d;
	}

	return ret;
}

// Multi
class MultiImageProvider : public ImageProvider {
public:
	std::vector<std::unique_ptr<ImageProvider>> providers_;
	unsigned layers_ {};
	unsigned mips_ {};
	bool cubemap_ {};
	nytl::Vec3ui size_ {};
	vk::Format format_ = vk::Format::undefined;

public:
	vk::Format format() const noexcept override { return format_; }
	unsigned mipLevels() const noexcept override { return mips_; }
	unsigned layers() const noexcept override { return layers_; }
	nytl::Vec3ui size() const noexcept override { return size_; }
	bool cubemap() const noexcept override { return cubemap_; }

	u64 read(nytl::Span<std::byte> data, unsigned mip = 0, unsigned layer = 0) const override {
		dlg_assert(mip < mips_ && layer < layers_);
		return providers_[layer]->read(data, mip, 0);
	}

	nytl::Span<const std::byte> read(unsigned mip = 0, unsigned layer = 0) const override {
		dlg_assert(mip < mips_ && layer < layers_);
		return providers_[layer]->read(mip, 0);
	}
};

std::unique_ptr<ImageProvider> read(nytl::Span<const char* const> paths, bool cubemap) {
	auto ret = std::make_unique<MultiImageProvider>();
	auto first = true;
	ret->layers_ = paths.size();
	ret->cubemap_ = cubemap;

	for(auto& path : paths) {
		auto provider = read(path);
		if(!provider) {
			return {};
		}

		if(first) {
			first = false;
			ret->format_ = provider->format();
			ret->size_ = provider->size();
			ret->mips_ = provider->mipLevels();
		} else {
			// Make sure that this image has the same properties as the
			// other images
			auto isize = provider->size();
			if(isize != ret->size_) {
				auto msg = dlg::format(
					"LayeredImageProvider: Image layer has different size:"
					"\n\tFirst image had size {}"
					"\n\t'{}' has size {}", ret->size_, path, isize);
				throw std::runtime_error(msg);
			}

			auto iformat = provider->format();
			if(iformat != ret->format_) {
				auto msg = dlg::format(
					"LayeredImageProvider: Image layer has different format:"
					"\n\tFirst image had format {}"
					"\n\t'{}' has format {}",
					(int) ret->format_, path, (int) iformat);
				throw std::runtime_error(msg);
			}

			auto imips = provider->mipLevels();
			if(imips != ret->mips_) {
				auto msg = dlg::format(
					"LayeredImageProvider: Image layer has different mip count:"
					"\n\tFirst image had mip count {}"
					"\n\t'{}' has mip count {}",
					(int) ret->mips_, path, (int) imips);
				throw std::runtime_error(msg);
			}
		}

		dlg_assertlm(dlg_level_warn, provider->layers() == 1u,
			"{} layers will not be accessible", provider->layers() - 1);
		ret->providers_.push_back(std::move(provider));
	}

	return ret;
}

} // namespace tkn
