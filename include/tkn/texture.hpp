#pragma once

#include <tkn/defer.hpp>
#include <tkn/image.hpp>
#include <tkn/render.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <nytl/stringParam.hpp>
#include <nytl/span.hpp>
#include <nytl/vec.hpp>

// NOTE: none of the functions/classes have support for compressed/packed
// or depth/stencil formats
// TODO: honestly, tkn::Texture should just be functions (basically
//  all constructors/init/create as functions).
//  And then maybe create a high level texture class, storing
//  formats, size, layers, mip etc?
//  Unify with fill api at the bottom.

namespace tkn {

// NOTE: stb offers a function to autodetect whether the image is hdr.
// We don't use this here (e.g. use a optional<bool> hdr parameter)
// since the caller knows what it wants the texture for and has to adapt
// it's pipelines (so getting images that return sampled values >1 would
// be unexpected).
// If a pipeline supports and expects both hdr and ldr images properly,
// the caller can still use stbi_is_hdr(filename) manually before calling this.
// The default format used when 'hdr = true' is passed is rgba16f, otherwise
// will use rgba8.

// returns whether the given format an hold high dynamic range data, i.e.
// has a float format and can store values >1.0.
bool isHDR(vk::Format);
bool isSRGB(vk::Format);
vk::Format toggleSRGB(vk::Format);
vk::ImageType minImageType(vk::Extent3D, unsigned minDim = 1u);
vk::ImageViewType minImageViewType(vk::Extent3D, unsigned layers,
	bool cubemap, unsigned minDim = 1u);

// Vulkan enums declared via static to not include vulkan headers
struct TextureCreateParams {
	// default usage flags: sampled, input attachment, transfer dst
	static const vk::ImageUsageFlags defaultUsage;

	// The format of the created image.
	// If left empty, will simply use the format of the provided image.
	std::optional<vk::Format> format {};

	// If given, will force interpret the data as srgb/non-srgb.
	// No effect if the data is not unorm.
	std::optional<bool> srgb {};

	// Usage flags of the created image.
	vk::ImageUsageFlags usage = defaultUsage;

	// number of mipmap levels to create
	// none, auto: create as many mipmaps as the image data provides
	// Otherwise creates the specified number of mipmap levels.
	// Specify 0 to create a full mipmap chain.
	std::optional<unsigned> mipLevels {};

	// only relevant if mipmaps > 1
	// none, auto: only fill mimpmaps if image provides it
	// false: don't fill any mipmaps levels
	// true: use mipmap levels from image data but if there are none,
	//   create them manually (via vulkan blits)
	std::optional<bool> fillMipmaps {};

	// Whether to create a cubemap image and view.
	// none, auto: will create a cubemap if the image provider signals
	//   the image should be interpreted as cubemap.
	// true: there have to be 6 faces in the provided image and
	//   layerCount in ViewRange must be 6.
	// false: will simply interpret all layers as layers.
	std::optional<bool> cubemap {};

	// When cubemap is true and there is more than one layer (either
	// explicitly set or by leaving layerCount to 0), the device
	// must have the 'imageCubeArray' feature enabled.
	struct ViewRange {
		unsigned baseMipLevel = 0;
		unsigned baseArrayLayer = 0;
		unsigned layerCount = 0; // 0: all
		unsigned levelCount = 0; // 0: all
	} view;

	// Minimum dimension of image and image view types.
	// Useful to make even 1-height images 2D textures or
	// 1-depth textured 3D textures.
	unsigned minTypeDim {2};
};

// Basically just a ViewableImage that allows for deferred uploading.
// Always allocated on deviceLocal memory.
class [[deprecated("Use the standalone functions")]] Texture {
public:
	struct InitData {
		vpp::ViewableImage::InitData initImage {};

		vpp::SubBuffer stageBuf;
		vpp::SubBuffer::InitData initStageBuf {};

		vpp::Image stageImage;
		vpp::Image::InitData initStageImage {};

		vk::Format dstFormat;
		std::unique_ptr<ImageProvider> image;
		vk::Format dataFormat; // may have toggles srgb
		unsigned levels {}; // how many levels in total, >0
		unsigned layers {}; // how many layers/faces in total, >0
		unsigned fillLevels {}; // how many pre-filled levels, >0
		bool genLevels {}; // whether to generate remaining levels
		bool cubemap {}; // whether image is cubemap

		TextureCreateParams::ViewRange view;
	};

public:
	Texture() = default;
	Texture(vpp::ViewableImage&& img) : image_(std::move(img)) {}

	Texture(const vpp::Device& dev, std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& = {});
	Texture(const WorkBatcher&, std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& = {});
	Texture(InitData&, const WorkBatcher&, std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& = {});

	void init(InitData&, const WorkBatcher&);

	auto& device() const { return image_.device(); }
	auto& viewableImage() { return image_; }
	const auto& viewableImage() const { return image_; }
	const auto& image() const { return image_.image(); }
	const auto& imageView() const { return image_.imageView(); }
	const vk::ImageView& vkImageView() const { return image_.vkImageView(); }
	const vk::Image& vkImage() const { return image_.vkImage(); }

protected:
	vpp::ViewableImage image_;
};

// new, low-level experimental api
struct FillData {
	vpp::Image::InitData initStageImage;
	vpp::SubBuffer::InitData initStageBuffer;

	// Total number of writable levels in the target image.
	// If this is more than what source provides, will generate
	// mipmaps for the rest.
	unsigned numLevels;
	bool copyToStageImage {};
	bool cpuConversion {};
	vpp::Image stageImage;
	vpp::SubBuffer stageBuffer;

	vk::Image target;
	vk::Format dstFormat;
	vk::Format srcFormat;
	std::unique_ptr<ImageProvider> source;

	SyncScope dstScope = SyncScope::fragmentSampled();
};

// FillData must remain valid until the CommandBuffer has finished execution.
// Responsibility of the caller to make sure the image can be filled with all
// the data the given provider provides and that the requires transfer/blit
// usage flags in the target image are set. Will blit if needed.
FillData createFill(const WorkBatcher& wb, const vpp::Image&, vk::Format,
	std::unique_ptr<ImageProvider>, unsigned maxNumLevels,
	std::optional<bool> forceSRGB = {});
void doFill(FillData&, vk::CommandBuffer cb);

struct TextureInitData {
	vpp::ViewableImage::InitData initImage {};
	FillData fill {};

	unsigned numLevels {}; // how many levels in total, >0
	bool cubemap {}; // whether image is cubemap (compatible)
	vpp::Image image;

	vk::ImageViewType viewType;
	TextureCreateParams::ViewRange view {};
};

TextureInitData createTexture(const WorkBatcher&,
	std::unique_ptr<ImageProvider> img, const TextureCreateParams& = {});
vpp::Image initImage(TextureInitData&, const WorkBatcher&);
vpp::ViewableImage initTexture(TextureInitData&, const WorkBatcher&);

vpp::ViewableImage buildTexture(const WorkBatcher&,
	std::unique_ptr<ImageProvider> img, const TextureCreateParams& = {});
vpp::ViewableImage buildTexture(const vpp::Device&,
	std::unique_ptr<ImageProvider> img, const TextureCreateParams& = {});

} // namespace tkn
