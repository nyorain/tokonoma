#pragma once

#include <nytl/stringParam.hpp>
#include <functional>

#include <nytl/vec.hpp>
#include <vpp/fwd.hpp>

#include <cstddef>
#include <memory>

namespace doi {

/// Provides information and data of an image.
/// Abstraction allows to load/save/copy images as flexibly as possible.
class ImageProvider {
public:
	/// The size of the image.
	virtual nytl::Vec2ui size() const = 0;

	/// The format of the image.
	/// The data from 'read' will be in this format.
	virtual vk::Format format() const = 0;

	/// The number of layers the image has.
	virtual unsigned layers() const { return 1u; }

	/// The number of mipmap levels the image has.
	virtual unsigned mipLevels() const { return 1u; }

	/// The number of faces the image has.
	/// Usually 1 or 6 (cubemap).
	virtual unsigned faces() const { return 1u; }

	/// The rationale behind providing 2 apis for getting data from the provider
	/// is to avoid as many copies as possible.
	/// For some providers (e.g. file on disk) it's more comfortable to read
	/// data into a given buffer so they don't have to allocate memory
	/// and additionally copy the data.
	/// For other providers (e.g. file in ram/mapped device memory) it's more
	/// comfortable to simply return the data span instead of allocating
	/// data and copying it.

	/// Reads one full, tighly packed 2D image from the given face, mip, layer.
	/// The returned span is only guaranteed to be valid until the next read
	/// call.
	/// Face, mip, layer must be below the respectively advertised counts.
	/// Returns empty span on error. Also allowed to throw.
	virtual nytl::Span<std::byte> read(
		unsigned face = 0, unsigned mip = 0, unsigned layer = 0) = 0;

	/// Copies one full, tightly packed 2D image from the given face, mip, layer
	/// into the provided data buffer. The provided data buffer must be
	/// large enough to fit the data, partial reading not supported.
	/// Face, mip, layer must be below the respectively advertised counts.
	/// Return false on error. Also allowed to throw.
	virtual bool read(nytl::Span<std::byte> data,
		unsigned face = 0, unsigned mip = 0, unsigned layer = 0) = 0;
};

enum class ReadError {
	none,
	cantOpen,
	invalidType,
	internal,
	unexpectedEnd,
	invalidEndianess,

	ktxInvalidFormat, // ktx: unsupported/unknown format
	ktxDepth, // ktx: format has depth
};

/// Backend/Format-specific functions. Create image provider implementations
/// into the given unique ptr on success.
ReadError readKtx(nytl::StringParam path, std::unique_ptr<ImageProvider>&);
ReadError readJpeg(nytl::StringParam path, std::unique_ptr<ImageProvider>&);
ReadError readPng(nytl::StringParam path, std::unique_ptr<ImageProvider>&);

/// STB babckend is a fallback since it supports additional formats.
/// For !hdr, will always return r8g8b8a8Unorm as image format, the caller must
/// know whether the image holds srgb data. For hdr, always returns
/// vk::Format::r32g32b32a32Sfloat format.
std::unique_ptr<ImageProvider> readStb(nytl::StringParam path,
	bool hdr = false);

/// Tries to find the matching backend/loader for the image file at the
/// given path. If no format/backend succeeds, the returned unique ptr will be
/// empty. The hdr parameter is just for stb fallback.
std::unique_ptr<ImageProvider> read(nytl::StringParam path, bool hdr = false);

/// More limited in-memory representation of an image.
struct Image {
	nytl::Vec2ui size {};
	vk::Format format {};
	std::unique_ptr<std::byte[]> data;
};

/// Reads the first layer, mip, face of the given image provider
/// and stores them in memory as Image. On error, an empty image
/// is returned (e.g. the image provider has unsupported format).
Image readImage(ImageProvider&);

/// Uses stb to read the image at the given path.
/// For !hdr, will always return r8g8b8a8Unorm as image format, the caller must
/// know whether the image holds srgb data. For hdr, always returns
/// vk::Format::r32g32b32a32Sfloat format.
Image readImageStb(nytl::StringParam path, bool hdr = false);

/// Fully reads the image at the given path into memory and returns
/// the image. Returns an empty image on error.
/// The hdr parameter is just for stb fallback.
Image readImage(nytl::StringParam path, bool hdr = false);

/// Transforms the given image into an image provider implementation.
/// The provider will take ownership of the image.
std::unique_ptr<ImageProvider> wrap(Image&& image);

enum class WriteError {
	none,
	cantOpen,
	cantWrite,
	readError, // image provider failed reading/returned unexpected size
	invalidFormat, // unexpected/unsupported format
	internal,
};

WriteError writeKtx(nytl::StringParam path, ImageProvider&);

} // namespace doi
