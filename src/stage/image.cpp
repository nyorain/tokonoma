#include <stage/image.hpp>
#include <stage/types.hpp>
#include <stage/util.hpp>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>
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

using namespace doi::types;

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
		dlg_warn("Failed to open texture file {}", path);

		std::string err = "Could not load image from ";
		err += path;
		err += ": ";
		err += stbi_failure_reason();
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
	Image image;

public:
	nytl::Vec2ui size() const override { return image.size; }
	vk::Format format() const override { return image.format; }

	nytl::Span<std::byte> read(unsigned face = 0, unsigned mip = 0,
			unsigned layer = 0) override {
		dlg_assert(face == 0 && mip == 0 && layer == 0);
		auto byteSize = size().x * size().y * vpp::formatSize(format());
		return {image.data.get(), image.data.get() + byteSize};
	}

	bool read(nytl::Span<std::byte> data,
			unsigned face = 0, unsigned mip = 0, unsigned layer = 0) override {
		dlg_assert(face == 0 && mip == 0 && layer == 0);
		auto byteSize = size().x * size().y * vpp::formatSize(format());
		dlg_assert(data.size() >= byteSize);
		std::memcpy(data.data(), image.data.get(), byteSize);
		return true;
	}
};

std::unique_ptr<ImageProvider> wrap(Image&& image) {
	auto ret = std::make_unique<MemImageProvider>();
	ret->image = std::move(image);
	return ret;
}

} // namespace doi
