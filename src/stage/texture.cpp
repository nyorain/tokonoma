#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/types.hpp>
#include <stage/image.hpp>

#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/debug.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/queue.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>

// stbi supports more format (especially hdr) but our own jpeg/png
// loaders (using the libraries) are *way* faster.
constexpr bool tryStageImage = true;

// TODO(optimization): we could compare requested format with formats supported
//   by stbi instead of blitting even e.g. for r8 formats (which are supported
//   by stbi by passing channels=1)
// TODO: support for compressed formats, low priority for now
//   supporit ktx and dds formats, shouldn't be too hard
// TODO: check if blit is supported in format flags

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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC // needed, otherwise we mess with other usages
#pragma GCC diagnostic ignored "-Wunused-function" // static functions
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace doi {
using namespace doi::types;

const vk::ImageUsageFlags TextureCreateParams::defaultUsage =
	vk::ImageUsageBits::sampled |
	vk::ImageUsageBits::transferDst |
	vk::ImageUsageBits::inputAttachment;
const vk::Format TextureCreateParams::defaultFormat = vk::Format::r8g8b8a8Srgb;

std::tuple<std::unique_ptr<std::byte[]>, vk::Extent2D> loadImage(
		nytl::StringParam filename, unsigned channels, bool hdr) {
	if(tryStageImage && !hdr && channels == 4) {
		Image image;
		auto err = load(filename, image);
		if(err == Error::none) {
			vk::Extent2D ext{image.width, image.height};
			return {std::move(image.data), ext};
		} else {
			dlg_debug("Loading image with internal loader failed: Error {}",
				(unsigned) err);
		}
	}

	dlg_assertlm(dlg_level_debug, stbi_is_hdr(filename.c_str()) == hdr,
		"texture '{}' requires stbi hdr conversion", filename);

	int width, height, ch;
	std::byte* data;
	if(hdr) {
		auto fd = stbi_loadf(filename.c_str(), &width, &height, &ch, channels);
		data = reinterpret_cast<std::byte*>(fd);
	} else {
		auto cd = stbi_load(filename.c_str(), &width, &height, &ch, channels);
		data = reinterpret_cast<std::byte*>(cd);
	}

	if(!data) {
		dlg_warn("Failed to open texture file {}", filename);

		std::string err = "Could not load image from ";
		err += filename;
		err += ": ";
		err += stbi_failure_reason();
		throw std::runtime_error(err);
	}

	dlg_assert(width > 0 && height > 0);

	vk::Extent2D extent;
	extent.width = width;
	extent.height = height;
	return {std::unique_ptr<std::byte[]>(data), extent};
}

// texture
Texture::Texture(InitData& data, const WorkBatcher& batcher,
		nytl::StringParam file, const TextureCreateParams& params) {
	auto hdr = isHDR(params.format);
	auto srgb = isSRGB(params.format);
	auto [imageData, imageSize] = loadImage(file, 4, hdr);

	auto dataFormat = hdr ?
		vk::Format::r32g32b32a32Sfloat :
		srgb ?
			vk::Format::r8g8b8a8Srgb :
			vk::Format::r8g8b8a8Unorm;
	std::vector<std::unique_ptr<std::byte[]>> layers;
	layers.emplace_back(std::move(imageData));
	*this = {data, batcher, std::move(layers), dataFormat,
		imageSize, params};
}

Texture::Texture(InitData& data, const WorkBatcher& batcher,
		nytl::Span<const char* const> files,
		const TextureCreateParams& params) {
	auto hdr = isHDR(params.format);
	auto srgb = isSRGB(params.format);
	auto dataFormat = hdr ?
		vk::Format::r32g32b32a32Sfloat :
		srgb ?
			vk::Format::r8g8b8a8Srgb :
			vk::Format::r8g8b8a8Unorm;

	std::vector<std::unique_ptr<std::byte[]>> layers;
	layers.reserve(files.size());
	vk::Extent2D size;

	for(auto filename : files) {
		auto [data, extent] = loadImage(filename, 4, hdr);
		dlg_assert(extent.width > 0 && extent.height > 0);
		if(size.width == 0 || size.height == 0) {
			size.width = extent.width;
			size.height = extent.height;
		} else if(extent.width != size.width || extent.height != size.height) {
			std::string msg = "Images for image array have different sizes:";
			msg += "\n\tFirst image had size (";
			msg += std::to_string(size.width);
			msg += ",";
			msg += std::to_string(size.height);
			msg += "), while '" + std::string(filename) + "' has size (";
			msg += std::to_string(extent.width);
			msg += ",";
			msg += std::to_string(extent.height);
			msg += ").";
			throw std::runtime_error(msg);
		}

		layers.push_back(std::move(data));
	}

	*this = {data, batcher, std::move(layers), dataFormat, size, params};
}

Texture::Texture(InitData& data, const WorkBatcher& batcher,
		std::vector<std::unique_ptr<std::byte[]>> dataLayers,
		vk::Format dataFormat, const vk::Extent2D& size,
		const TextureCreateParams& params) {
	data = {};
	auto layers = dataLayers.size();
	dlg_assert(!params.cubemap || layers == 6u);
	dlg_assert(layers > 0);

	auto blit = dataFormat != params.format;
	vk::FormatFeatureFlags features {};
	if(blit) {
		features |= vk::FormatFeatureBits::blitDst;
	}

	auto usage = params.usage | vk::ImageUsageBits::transferDst;
	if(params.mipmaps) {
		usage |= vk::ImageUsageBits::transferSrc;
	}

	auto levels = 1u;
	if(params.mipmaps) {
		levels = vpp::mipmapLevels(size);
	}

	auto info = vpp::ViewableImageCreateInfo(
		params.format, vk::ImageAspectBits::color, {size.width, size.height},
		usage, vk::ImageTiling::optimal, levels);
	info.img.arrayLayers = layers;
	if(params.cubemap) {
		info.img.flags = vk::ImageCreateBits::cubeCompatible;
		data.cubemap = true;
	}

	dlg_assert(vpp::supported(batcher.dev, info.img));
	auto devBits = batcher.dev.deviceMemoryTypes();
	auto hostBits = batcher.dev.hostMemoryTypes();
	image_ = {data.initImage, batcher.dev, info.img, devBits,
		&batcher.alloc.memDevice};

	data.data = std::move(dataLayers);
	data.dataFormat = dataFormat;
	data.dstFormat = params.format;
	data.levels = levels;
	data.size = {size.width, size.height};
	if(blit) {
		auto usage = vk::ImageUsageBits::transferSrc;
		auto info = vpp::ViewableImageCreateInfo(dataFormat,
			vk::ImageAspectBits::color, {size.width, size.height},
			usage, vk::ImageTiling::linear);
		info.img.arrayLayers = layers;
		info.img.initialLayout = vk::ImageLayout::preinitialized;
		dlg_assert(vpp::supported(batcher.dev, info.img));
		data.stageImage = {data.initStageImage, batcher.dev, info.img,
			hostBits, &batcher.alloc.memStage};
		vpp::nameHandle(data.stageImage, "Texture:stageImage");
	} else {
		auto dataSize = layers * vpp::formatSize(dataFormat) *
			size.width * size.height;
		auto usage = vk::BufferUsageBits::transferSrc;
		data.stageBuf = {data.initStageBuf, batcher.alloc.bufStage, dataSize,
			usage, hostBits};
	}
}

Texture::Texture(const WorkBatcher& wb, nytl::StringParam file,
	const TextureCreateParams& params) :
		Texture(wb, nytl::Span<const char* const>{{file.c_str()}}, params) {
}

Texture::Texture(const WorkBatcher& wb,
		nytl::Span<const std::byte> data,
		vk::Format dataFormat, const vk::Extent2D& size,
		const TextureCreateParams& params) {
	auto dataSize = size.width * size.height * vpp::formatSize(dataFormat);
	dlg_assert(data.size() == dataSize);

	auto unique = std::make_unique<std::byte[]>(dataSize);
	std::memcpy(unique.get(), data.data(), dataSize);
	std::vector<std::unique_ptr<std::byte[]>> faces;
	faces.emplace_back(std::move(unique));
	*this = {wb, std::move(faces), dataFormat, size, params};
}

Texture::Texture(const WorkBatcher& wb, nytl::Span<const char* const> files,
		const TextureCreateParams& params) {
	auto& qs = wb.dev.queueSubmitter();
	auto cb = wb.dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});

	auto cwb = wb;
	cwb.cb = cb;

	InitData data;
	*this = {data, cwb, files, params};
	init(data, cwb);

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));
}

Texture::Texture(const WorkBatcher& wb,
		std::vector<std::unique_ptr<std::byte[]>> dataLayers,
		vk::Format dataFormat, const vk::Extent2D& size,
		const TextureCreateParams& params) {
	auto& qs = wb.dev.queueSubmitter();
	auto cb = wb.dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});

	auto cwb = wb;
	cwb.cb = cb;

	InitData data;
	*this = {data, cwb, std::move(dataLayers), dataFormat, size, params};
	init(data, cwb);

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));
}

Texture::Texture(InitData& initData, const WorkBatcher& batcher,
		nytl::Span<const std::byte> data,
		vk::Format dataFormat, const vk::Extent2D& size,
		const TextureCreateParams& params) {
	auto dataSize = size.width * size.height * vpp::formatSize(dataFormat);
	dlg_assert(data.size() == dataSize);

	auto unique = std::make_unique<std::byte[]>(dataSize);
	std::memcpy(unique.get(), data.data(), dataSize);
	std::vector<std::unique_ptr<std::byte[]>> faces;
	faces.emplace_back(std::move(unique));
	*this = {initData, batcher, std::move(faces), dataFormat, size, params};
}

void Texture::init(InitData& data, const WorkBatcher& batcher) {
	u32 layerCount = data.data.size();
	u32 levelCount = data.levels;

	vk::ImageViewCreateInfo ivi;
	ivi.format = data.dstFormat;
	ivi.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	ivi.subresourceRange.layerCount = data.data.size();
	ivi.subresourceRange.levelCount = data.levels;
	if(data.cubemap) {
		ivi.viewType = vk::ImageViewType::cube;
	} else if(layerCount > 1) {
		ivi.viewType = vk::ImageViewType::e2dArray;
	} else {
		ivi.viewType = vk::ImageViewType::e2d;
	}

	image_.init(data.initImage, ivi);

	// upload
	u32 width = data.size.x;
	u32 height = data.size.y;
	auto cb = batcher.cb;

	vk::ImageMemoryBarrier barrier;
	barrier.image = image_.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::transferDstOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferWrite;
	barrier.subresourceRange =
		{vk::ImageAspectBits::color, 0, levelCount, 0, layerCount};
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

	auto blit = data.dstFormat != data.dataFormat;
	auto lsize = width * height * vpp::formatSize(data.dataFormat);
	auto dataSize = layerCount * lsize;
	if(blit) {
		dlg_assert(data.stageImage);
		data.stageImage.init(data.initStageImage);

		barrier.image = data.stageImage;
		barrier.oldLayout = vk::ImageLayout::preinitialized;;
		barrier.srcAccessMask = vk::AccessBits::hostWrite;
		barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		barrier.subresourceRange =
			{vk::ImageAspectBits::color, 0, 1, 0, layerCount};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::host,
			vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

		for(auto& imgData : data.data) {
			vpp::fillMap(data.stageImage, data.dataFormat,
				{width, height, 1u}, {imgData.get(), lsize},
				{vk::ImageAspectBits::color});
		}

		vk::ImageBlit blit;
		blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.srcSubresource.layerCount = layerCount;
		blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.dstSubresource.layerCount = layerCount;
		blit.srcOffsets[1].x = width;
		blit.srcOffsets[1].y = height;
		blit.srcOffsets[1].z = 1;
		blit.dstOffsets[1].x = width;
		blit.dstOffsets[1].y = height;
		blit.dstOffsets[1].z = 1;

		// nearest is enough, we are not scaling in any way
		vk::cmdBlitImage(cb, data.stageImage,
			vk::ImageLayout::transferSrcOptimal, image_.image(),
			vk::ImageLayout::transferDstOptimal, {{blit}},
			vk::Filter::nearest);
	} else {
		data.stageBuf.init(data.initStageBuf);
		dlg_assert(data.stageBuf.buffer());

		std::vector<vk::BufferImageCopy> copies;
		copies.reserve(layerCount);

		auto map = data.stageBuf.memoryMap();
		dlg_assert(map.size() >= dataSize);
		auto span = map.span();
		auto offset = data.stageBuf.offset();
		auto layer = 0u;
		for(auto& imgData : data.data) {
			vk::BufferImageCopy copy {};
			copy.bufferOffset = offset;
			copy.imageExtent = {width, height, 1};
			copy.imageOffset = {};
			copy.imageSubresource.aspectMask = vk::ImageAspectBits::color;
			copy.imageSubresource.baseArrayLayer = layer++;
			copy.imageSubresource.layerCount = 1u;
			copy.imageSubresource.mipLevel = 0u;
			copies.push_back(copy);
			offset += lsize;
			doi::write(span, imgData.get(), lsize);
		}

		vk::cmdCopyBufferToImage(cb, data.stageBuf.buffer(), image_.image(),
			vk::ImageLayout::transferDstOptimal, copies);
	}

	// generate mipmaps
	barrier.image = image_.image();
	barrier.oldLayout = vk::ImageLayout::transferDstOptimal;
	barrier.srcAccessMask = vk::AccessBits::transferWrite;
	barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	barrier.dstAccessMask = vk::AccessBits::shaderRead;
	barrier.subresourceRange =
		{vk::ImageAspectBits::color, 0, levelCount, 0, layerCount};
	if(levelCount != 1) {
		// bring mip0 into transferSrc layout and set a barrier for initial
		// data transfer to complete
		barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		barrier.subresourceRange.levelCount = 1u;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});

		for(auto i = 1u; i < levelCount; ++i) {
			// std::max needed for end offsets when the texture is not
			// quadratic: then we would get 0 there although the mipmap
			// still has size 1
			vk::ImageBlit blit;
			blit.srcSubresource.baseArrayLayer = 0u;
			blit.srcSubresource.layerCount = layerCount;
			blit.srcSubresource.mipLevel = i - 1;
			blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.srcOffsets[1].x = std::max(width >> (i - 1), 1u);
			blit.srcOffsets[1].y = std::max(height >> (i - 1), 1u);
			blit.srcOffsets[1].z = 1u;

			blit.dstSubresource.layerCount = layerCount;
			blit.dstSubresource.mipLevel = i;
			blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.dstOffsets[1].x = std::max(width >> i, 1u);
			blit.dstOffsets[1].y = std::max(height >> i, 1u);
			blit.dstOffsets[1].z = 1u;

			vk::cmdBlitImage(cb, image_.image(),
				vk::ImageLayout::transferSrcOptimal, image_.image(),
				vk::ImageLayout::transferDstOptimal, {{blit}},
				vk::Filter::linear);

			// change layout of current mip level to transferSrc for next
			// mip level
			barrier.subresourceRange.baseMipLevel = i;
			vk::cmdPipelineBarrier(cb,
				vk::PipelineStageBits::transfer,
				vk::PipelineStageBits::transfer,
				{}, {}, {}, {{barrier}});
		}

		barrier.oldLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.srcAccessMask = vk::AccessBits::transferRead;
		barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		barrier.dstAccessMask = vk::AccessBits::shaderRead;
		barrier.subresourceRange =
			{vk::ImageAspectBits::color, 0, levelCount, 0, layerCount};
	}

	// transfer all mip levels to readable layout and set barrier to
	// wait for transfer to complete
	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::transfer,
		vk::PipelineStageBits::allCommands | vk::PipelineStageBits::topOfPipe,
		{}, {}, {}, {{barrier}});
}

// free utility
bool isHDR(vk::Format format) {
	// TODO: not sure about scaled formats, what are those?
	//  also what about packed formats? e.g. vk::Format::b10g11r11UfloatPack32?
	// TODO: even for snorm/unorm 16/32 bit formats we probably want to
	//  use the stbi hdr loader since otherwise we lose the precision
	//  when stbi converts to 8bit
	switch(format) {
		case vk::Format::r16Sfloat:
		case vk::Format::r16g16Sfloat:
		case vk::Format::r16g16b16Sfloat:
		case vk::Format::r16g16b16a16Sfloat:
		case vk::Format::r32Sfloat:
		case vk::Format::r32g32Sfloat:
		case vk::Format::r32g32b32Sfloat:
		case vk::Format::r32g32b32a32Sfloat:
		case vk::Format::r64Sfloat:
		case vk::Format::r64g64Sfloat:
		case vk::Format::r64g64b64Sfloat:
		case vk::Format::r64g64b64a64Sfloat:
			return true;
		default:
			return false;
	}
}

bool isSRGB(vk::Format format) {
	switch(format) {
		case vk::Format::r8Srgb:
		case vk::Format::r8g8Srgb:
		case vk::Format::r8g8b8Srgb:
		case vk::Format::r8g8b8a8Srgb:
			return true;
		default:
			return false;
	}
}

} // namespace doi
