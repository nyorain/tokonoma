#include <stage/image.hpp>
#include <stage/types.hpp>
#include <stage/util.hpp>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/span.hpp>
#include <vkpp/enums.hpp>
#include <vpp/imageOps.hpp>
#include <dlg/dlg.hpp>

#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>

// make stbi std::unique_ptr<std::byte[]> compatible
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

namespace doi {

// ImageProvider api
std::unique_ptr<ImageProvider> readStb(nytl::StringParam path, bool hdr) {
	auto img = readImageStb(path, hdr);
	if(!img.data) {
		return {};
	}

	return wrap(std::move(img));
}

// TODO: the other loaders should respect hdr as well i guess...
std::unique_ptr<ImageProvider> read(nytl::StringParam path, bool hdr) {
	std::unique_ptr<ImageProvider> reader;
	bool triedpng = false;
	bool triedjpg = false;
	bool triedktx = false;

	// first try detecting the type based of suffix
	if(has_suffix(path, ".png")) {
		triedpng = true;
		if(readPng(path, reader) == ReadError::none) {
			return reader;
		}
	} else if(has_suffix(path, ".jpg") || has_suffix(path, ".jpeg")) {
		triedjpg = true;
		if(readJpeg(path, reader) == ReadError::none) {
			return reader;
		}
	} else if(has_suffix(path, ".ktx")) {
		triedktx = true;
		if(readKtx(path, reader) == ReadError::none) {
			return reader;
		}
	}

	// then just try all remaining loaders
	if(!triedpng) {
		if(readPng(path, reader) == ReadError::none) {
			return reader;
		}
	}
	if(!triedjpg) {
		if(readJpeg(path, reader) == ReadError::none) {
			return reader;
		}
	}
	if(!triedktx) {
		if(readKtx(path, reader) == ReadError::none) {
			return reader;
		}
	}

	// stb is our last fallback
	return readStb(path, hdr);
}

// Image api
Image readImageStb(nytl::StringParam path, bool hdr) {
	constexpr auto channels = 4u; // TODO: make configurable
	int width, height, ch;
	std::byte* data;
	Image ret;
	if(hdr) {
		auto fd = stbi_loadf(path.c_str(), &width, &height, &ch, channels);
		data = reinterpret_cast<std::byte*>(fd);
		ret.format = vk::Format::r32g32b32a32Sfloat;
	} else {
		auto cd = stbi_load(path.c_str(), &width, &height, &ch, channels);
		data = reinterpret_cast<std::byte*>(cd);
		ret.format = vk::Format::r8g8b8a8Unorm;
	}

	if(!data) {
		std::string err = "STB could not load image from ";
		err += path;
		err += ": ";
		err += stbi_failure_reason();
		dlg_warn("{}", err);
		return ret;
	}

	ret.data.reset(data);
	ret.size.x = width;
	ret.size.y = height;
	return ret;
}


Image readImage(nytl::StringParam path, bool hdr) {
	auto provider = read(path, hdr);
	if(!provider) {
		return {};
	}

	return readImage(*provider);
}

Image readImage(ImageProvider& provider) {
	dlg_assertlm(dlg_level_debug, provider.faces() == 1,
		"readImage: discarding {} faces", provider.faces() - 1);
	dlg_assertlm(dlg_level_debug, provider.layers() == 1,
		"readImage: discarding {} layers", provider.layers() - 1);
	dlg_assertlm(dlg_level_debug, provider.mipLevels() == 1,
		"readImage: discarding {} mip levels", provider.mipLevels() - 1);

	Image ret;
	ret.format = provider.format();
	ret.size = provider.size();

	auto byteSize = ret.size.x * ret.size.y * vpp::formatSize(ret.format);
	ret.data = std::make_unique<std::byte[]>(byteSize);
	if(!provider.read({ret.data.get(), ret.data.get() + byteSize})) {
		return {};
	}

	return ret;
}

class MemImageProvider : public ImageProvider {
public:
	struct MipLayerFace {
		std::unique_ptr<std::byte[]> owned;
		const std::byte* ref;
	};

	std::vector<MipLayerFace> data_;
	unsigned layers_;
	unsigned faces_;
	unsigned mips_;
	nytl::Vec2ui size_;
	vk::Format format_;

public:
	u64 faceSize() const {
		return size_.x * size_.y * vpp::formatSize(format_);
	}

	unsigned layers() const override { return layers_; }
	unsigned faces() const override { return faces_; }
	unsigned mipLevels() const override { return mips_; }

	nytl::Vec2ui size() const override { return size_; }
	vk::Format format() const override { return format_; }

	nytl::Span<const std::byte> read(unsigned mip = 0, unsigned layer = 0,
			unsigned face = 0) override {
		dlg_assert(face < faces() && mip < mipLevels() && layer < layers());
		auto id = mip * (layers_ * faces_) + layer * faces_ + face;
		return {data_[id].ref, data_[id].ref + faceSize()};
	}

	bool read(nytl::Span<std::byte> data, unsigned mip = 0,
			unsigned layer = 0, unsigned face = 0) override {
		dlg_assert(face < faces() && mip < mipLevels() && layer < layers());
		auto id = mip * (layers_ * faces_) + layer * faces_ + face;

		auto byteSize = faceSize();
		dlg_assert(u64(data.size()) >= byteSize);
		std::memcpy(data.data(), data_[id].ref, byteSize);
		return true;
	}
};

std::unique_ptr<ImageProvider> wrap(Image&& image) {
	auto ret = std::make_unique<MemImageProvider>();
	ret->layers_ = ret->faces_ = ret->mips_ = 1u;
	ret->format_ = image.format;
	ret->size_ = image.size;
	auto& data = ret->data_.emplace_back();
	data.owned = std::move(image.data);
	data.ref = data.owned.get();
	return ret;
}

std::unique_ptr<ImageProvider> wrap(nytl::Vec2ui size, vk::Format format,
		nytl::Span<const std::byte> span) {
	dlg_assert(span.size() >= size.x * size.y * vpp::formatSize(format));

	auto ret = std::make_unique<MemImageProvider>();
	ret->layers_ = ret->faces_ = ret->mips_ = 1u;
	ret->format_ = format;
	ret->size_ = size;
	auto& data = ret->data_.emplace_back();
	data.ref = span.data();
	return ret;
}

std::unique_ptr<ImageProvider> wrap(nytl::Vec2ui size, vk::Format format,
		unsigned mips, unsigned layers, unsigned faces,
		nytl::Span<std::unique_ptr<std::byte[]>> data) {
	dlg_assert(data.size() == mips * layers * faces);
	auto ret = std::make_unique<MemImageProvider>();
	ret->mips_ = mips;
	ret->layers_ = layers;
	ret->faces_ = faces;
	ret->format_ = format;
	ret->size_ = size;
	ret->data_.reserve(data.size());
	for(auto& d : data) {
		auto& rdi = ret->data_.emplace_back();
		rdi.owned = std::move(d);
		rdi.ref = rdi.owned.get();
	}

	return ret;
}

std::unique_ptr<ImageProvider> wrap(nytl::Vec2ui size, vk::Format format,
		unsigned mips, unsigned layers, unsigned faces,
		nytl::Span<const std::byte* const> data) {
	dlg_assert(data.size() == mips * layers * faces);
	auto ret = std::make_unique<MemImageProvider>();
	ret->mips_ = mips;
	ret->layers_ = layers;
	ret->faces_ = faces;
	ret->format_ = format;
	ret->size_ = size;
	ret->data_.reserve(data.size());
	for(auto& d : data) {
		auto& rdi = ret->data_.emplace_back();
		rdi.ref = d;
	}

	return ret;
}

// Layered
class LayeredImageProvider : public ImageProvider {
public:
	std::vector<std::unique_ptr<ImageProvider>> layers_;
	unsigned faces_ {};
	unsigned mips_ {};
	nytl::Vec2ui size_ {};
	vk::Format format_ = vk::Format::undefined;

public:
	vk::Format format() const override { return format_; }
	unsigned faces() const override { return faces_; }
	unsigned mipLevels() const override { return mips_; }
	unsigned layers() const override { return layers_.size(); }
	nytl::Vec2ui size() const override { return size_; }

	bool read(nytl::Span<std::byte> data, unsigned mip = 0, unsigned layer = 0,
			unsigned face = 0) override {
		dlg_assert(face < faces_ && mip < mips_ && layer < layers_.size());
		return layers_[layer]->read(data, face, mip, 0);
	}

	nytl::Span<const std::byte> read(unsigned mip = 0, unsigned layer = 0,
			unsigned face = 0) override {
		dlg_assert(face < faces_ && mip < mips_ && layer < layers_.size());
		return layers_[layer]->read(face, mip, 0);
	}
};

std::unique_ptr<ImageProvider> readLayers(nytl::Span<const char* const> paths,
		bool hdr) {
	auto ret = std::make_unique<LayeredImageProvider>();
	auto first = false;
	for(auto& path : paths) {
		auto provider = read(path, hdr);
		if(!provider) {
			return {};
		}

		if(first) {
			first = false;
			ret->format_ = provider->format();
			ret->size_ = provider->size();
			ret->faces_ = provider->faces();
			ret->mips_ = provider->mipLevels();
		} else {
			// Make sure that this image has the same properties as the
			// other images
			auto isize = provider->size();
			if(isize != ret->size_) {
				auto msg = dlg::format(
					"LayeredImageProvider: Image layer has different sizes:"
					"\n\tFirst image had size {}"
					"\n\t'{}' has size {}", ret->size_, path, isize);
				throw std::runtime_error(msg);
			}

			auto iformat = provider->format();
			if(iformat != ret->format_) {
				auto msg = dlg::format(
					"LayeredImageProvider: Image layer has different formats:"
					"\n\tFirst image had format {}"
					"\n\t'{}' has format {}",
					(int) ret->format_, path, (int) iformat);
				throw std::runtime_error(msg);
			}

			auto imips = provider->mipLevels();
			if(imips != ret->mips_) {
				auto msg = dlg::format(
					"LayeredImageProvider: Image layer has different mip counts:"
					"\n\tFirst image had mip count {}"
					"\n\t'{}' has mip count {}",
					(int) ret->mips_, path, (int) imips);
				throw std::runtime_error(msg);
			}

			auto ifaces = provider->faces();
			if(ifaces != ret->faces_) {
				auto msg = dlg::format(
					"LayeredImageProvider: Image layer has different face count:"
					"\n\tFirst image had face count {}"
					"\n\t'{}' has face count {}",
					(int) ret->faces_, path, (int) ifaces);
				throw std::runtime_error(msg);
			}
		}

		ret->layers_.push_back(std::move(provider));
	}

	return ret;
}

} // namespace doi
