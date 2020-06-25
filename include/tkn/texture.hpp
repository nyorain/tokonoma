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

namespace tkn {

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

	const vpp::Device* dev;
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
FillData createFill(WorkBatcher& wb, const vpp::Image&, vk::Format,
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

TextureInitData createTexture(WorkBatcher&,
	std::unique_ptr<ImageProvider> img, const TextureCreateParams& = {});
vpp::Image initImage(TextureInitData&, WorkBatcher&);
vpp::ViewableImage initTexture(TextureInitData&, WorkBatcher&);

vpp::ViewableImage buildTexture(WorkBatcher&,
	std::unique_ptr<ImageProvider> img, const TextureCreateParams& = {});
vpp::ViewableImage buildTexture(const vpp::Device&,
	std::unique_ptr<ImageProvider> img, const TextureCreateParams& = {});

} // namespace tkn
