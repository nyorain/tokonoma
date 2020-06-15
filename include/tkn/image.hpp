#pragma once

#include <tkn/file.hpp>
#include <tkn/types.hpp>
#include <nytl/stringParam.hpp>
#include <functional>

#include <nytl/vec.hpp>
#include <vpp/fwd.hpp>

#include <cstddef>
#include <memory>

namespace tkn {

class Stream;

/// Provides information and data of an image.
/// Abstraction allows to load/save/copy images as flexibly as possible.
/// Close to the vulkan model of an image.
/// We separate layers and depth here since mipmaps work differently for
/// both.
class ImageProvider {
public:
	virtual ~ImageProvider() = default;

	/// The size of the image. No component shall be zero, all >= 1.
	/// When the image has a depth > 1, the image must not have layers.
	virtual nytl::Vec3ui size() const noexcept = 0;

	/// The format of the image.
	/// The data from 'read' will be in this format.
	/// Shall never return vk::Format::undefined.
	virtual vk::Format format() const noexcept = 0;

	/// The number of layers the image has.
	/// Should always return a number >= 1.
	virtual unsigned layers() const noexcept { return 1u; }

	/// The number of mipmap levels the image has.
	/// Should always return a number >= 1.
	virtual unsigned mipLevels() const noexcept { return 1u; }

	/// Whether this image is a cubemap.
	/// Implementations might not be able to know this, it's to the
	/// best of their knowledge. If this is true, layers() must be
	/// a multiple of 6, >0 and contain the layers in packs of faces
	/// (i.e. face i, cubemap-layer j is at image layer 6j + i).
	virtual bool cubemap() const noexcept { return false; }

	/// The rationale behind providing 2 apis for getting data from the provider
	/// is to avoid as many copies as possible.
	/// For some providers (e.g. file on disk) it's more comfortable to read
	/// data into a given buffer so they don't have to allocate memory
	/// and additionally copy the data.
	/// For other providers (e.g. file in ram/mapped device memory) it's more
	/// comfortable to simply return the data span instead of allocating
	/// data and copying it.

	/// Reads one full, tighly packed 2D image from the given mip, layer.
	/// The returned span is only guaranteed to be valid until the next read
	/// call or until this object is desctructed., the data is not owned.
	/// Face, mip, layer must be below the respectively advertised counts.
	/// Throws on error.
	virtual nytl::Span<const std::byte> read(
		unsigned mip = 0, unsigned layer = 0) const = 0;

	/// Copies one full, tightly packed 2D image from the given mip, layer
	/// into the provided data buffer. The provided data buffer must have
	/// large enough to fit the data, partial reading not supported.
	/// Face, mip, layer must be below the respectively advertised counts.
	/// Throws on error. Returns the number of written bytes.
	virtual u64 read(nytl::Span<std::byte> data,
		unsigned mip = 0, unsigned layer = 0) const = 0;
};

/// Transforms the given image information into an image provider
/// implementation. The provider will only reference the given data, it
/// must stay valid until the provider is destroyed.
std::unique_ptr<ImageProvider> wrapImage(nytl::Vec3ui size, vk::Format format,
	nytl::Span<const std::byte> data);

/// Expects: data.size() == mips * layers, with data for mip m, layer
/// l at data[m * layer + l].
/// Will move the given data into the provider.
std::unique_ptr<ImageProvider> wrapImage(nytl::Vec3ui size, vk::Format format,
	unsigned mips, unsigned layers, nytl::Span<std::unique_ptr<std::byte[]>> data,
	bool cubemap = false);

/// Expects all mips and layers to be linearly lay out in data, i.e.
/// like done by vpp's imageOps, see vpp::tightTexelNumber.
std::unique_ptr<ImageProvider> wrapImage(nytl::Vec3ui size, vk::Format format,
	unsigned mips, unsigned layers, std::unique_ptr<std::byte[]> data,
	bool cubemap = false);
std::unique_ptr<ImageProvider> wrapImage(nytl::Vec3ui size, vk::Format format,
	unsigned mips, unsigned layers, nytl::Span<const std::byte> data,
	bool cubemap = false);

/// Expects: data.size() == mips * layers, with data for mip m, layer
/// l at data[m * layer + l].
/// Alternative to function above, there the provider will only reference the
/// given data, it must stay valid until the provider is destroyed.
std::unique_ptr<ImageProvider> wrapImage(nytl::Vec3ui size, vk::Format format,
	unsigned mips, unsigned layers, nytl::Span<const std::byte* const> data,
	bool cubemap = false);

enum class ReadError {
	none,
	cantOpen,
	invalidType,
	internal,
	unexpectedEnd,
	invalidEndianess,
	unsupportedFormat, // unsupported/unknown data format/feature
	cantRepresent, // can't represent the image via ImageProvider
	empty,
};

/// Backend/Format-specific functions. Create image provider implementations
/// into the given unique ptr on success. They expect binary streams.
/// They will move from the given stream only on success.
ReadError loadKtx(std::unique_ptr<Stream>&&, std::unique_ptr<ImageProvider>&);
ReadError loadJpeg(std::unique_ptr<Stream>&&, std::unique_ptr<ImageProvider>&);
ReadError loadPng(std::unique_ptr<Stream>&&, std::unique_ptr<ImageProvider>&);
ReadError loadExr(std::unique_ptr<Stream>&&, std::unique_ptr<ImageProvider>&,
	bool forceRGBA = true);

/// STB babckend is a fallback since it supports additional formats.
ReadError loadStb(std::unique_ptr<Stream>&&, std::unique_ptr<ImageProvider>&);

/// Tries to find the matching backend/loader for the image file at the
/// given path. If no format/backend succeeds, the returned unique ptr will be
/// empty.
/// 'ext' can contain a file extension (e.g. the full filename or just
/// something like ".png") to give a hint about the file type. The loader
/// will always try all image formats if the preferred one fails.
std::unique_ptr<ImageProvider> loadImage(std::unique_ptr<Stream>&&, std::string_view ext = "");
std::unique_ptr<ImageProvider> loadImage(nytl::StringParam filename);
std::unique_ptr<ImageProvider> loadImage(File&& file);
std::unique_ptr<ImageProvider> loadImage(nytl::Span<const std::byte> data);

/// Loads multiple images from the given paths and loads them as layers.
/// All images must have the same number of mip levels, same sizes
/// and same formats. Will always just consider the first layer from
/// each image.
std::unique_ptr<ImageProvider> loadImageLayers(nytl::Span<const char* const> paths,
	bool cubemap = false);

enum class WriteError {
	none,
	cantOpen,
	cantWrite,
	readError, // image provider failed reading/returned unexpected size
	unsupportedFormat, // unexpected/unsupported format
	internal,
};

WriteError writeKtx(nytl::StringParam path, const ImageProvider&);

/// Can only write 2D rgb or rgba images.
/// Will only write the first layer and mipmap.
WriteError writePng(nytl::StringParam path, const ImageProvider&);

// TODO: untested
/// Can write 2D hdr images.
WriteError writeExr(nytl::StringParam path, const ImageProvider&);


/// More limited in-memory representation of an image.
struct Image {
	nytl::Vec3ui size {};
	vk::Format format {};
	std::unique_ptr<std::byte[]> data;
};

/// Reads a specific layer, mip of the given image provider
/// and stores them in memory as Image. Does not catch exceptions
/// from the ImageProvider.
Image readImage(const ImageProvider&, unsigned mip = 0, unsigned layer = 0);
Image readImage(std::unique_ptr<Stream>&& stream, unsigned mip = 0, unsigned layer = 0);
Image readImageStb(std::unique_ptr<Stream>&& stream);

/// Transforms the given image into an image provider implementation.
/// The provider will take ownership of the image.
std::unique_ptr<ImageProvider> wrap(Image&& image);

} // namespace tkn
