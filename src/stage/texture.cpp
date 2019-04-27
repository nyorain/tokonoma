#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <vpp/vk.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/queue.hpp>
#include <vpp/formats.hpp>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>
#include <stdexcept>
#include <variant>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop

// TODO(optimization): we could compare requested format with formats supported
//   by stbi instead of blitting even e.g. for r8 formats (which are supported
//   by stbi by passing channels=1)
// TODO: support for compressed formats, low priority for now
//   supporit ktx and dds formats, shouldn't be too hard
// TODO: check if blit is supported in format flags

// NOTE: we currently require a blit by default for hdr images (since
// we read rgba32f values but store the texture in rgba16f).
// This is more expensive than simply uploading the data but saves
// a lot of memory (half the memory needed!) and most hdr images really
// only need 16 bit precision.

namespace doi {

std::tuple<std::byte*, vk::Extent3D> loadImage(nytl::StringParam filename,
		unsigned channels, bool hdr) {
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

	vk::Extent3D extent;
	extent.width = width;
	extent.height = height;
	extent.depth = 1u;
	return {data, extent};
}

vpp::ViewableImage loadTexture(const vpp::Device& dev,
		nytl::StringParam filename, bool srgb, bool hdr, bool mipmap) {
	dlg_assert(!hdr || !srgb);
	auto [data, extent] = loadImage(filename, 4, hdr);
	auto format = vk::Format::r8g8b8a8Unorm;
	auto dataFormat = vk::Format::r8g8b8a8Unorm;
	if(hdr) {
		format = vk::Format::r16g16b16a16Sfloat;
		dataFormat = vk::Format::r32g32b32a32Sfloat;
	} else if(srgb) {
		dataFormat = format = vk::Format::r8g8b8a8Srgb;
	}

	auto dataSize = extent.width * extent.height * vpp::formatSize(dataFormat);
	auto dspan = nytl::Span<const std::byte>(data, dataSize);
	auto img = loadTexture(dev, extent, format, dspan, dataFormat, mipmap);
	::free(data);
	return img;
}

vpp::ViewableImage loadTexture(const vpp::Device& dev,
		nytl::StringParam filename, vk::Format format, bool inputSrgb,
		bool mipmap) {
	auto hdr = isHDR(format);
	dlg_assert(!hdr || !inputSrgb);
	auto dataFormat = hdr ?
		vk::Format::r32g32b32a32Sfloat :
		inputSrgb ? vk::Format::r8g8b8a8Srgb : vk::Format::r8g8b8a8Unorm;

	auto [data, extent] = loadImage(filename, 4, hdr);
	auto dataSize = extent.width * extent.height * vpp::formatSize(dataFormat);
	auto dspan = nytl::Span<const std::byte>(data, dataSize);
	auto img = loadTexture(dev, extent, format, dspan, dataFormat, mipmap);
	::free(data);
	return img;
}

vpp::ViewableImage loadTexture(const vpp::Device& dev, vk::Extent3D extent,
		vk::Format format, nytl::Span<const std::byte> data,
		vk::Format dataFormat, bool mipmap) {
	if(dataFormat == vk::Format::undefined) {
		dataFormat = format;
	}

	auto usage = vk::ImageUsageBits::transferDst | vk::ImageUsageBits::sampled;
	auto levels = 1u;
	if(mipmap) {
		// complete mipmap chain as specified in
		// https://www.khronos.org/registry/vulkan/specs/1.1-extensions/html/chap11.html#resources-image-miplevel-sizing
		levels += std::floor(std::log2(std::max(extent.width, extent.height)));
		usage |= vk::ImageUsageBits::transferSrc;
	}

	vpp::ViewableImageCreateInfo info;
	auto devMem = dev.deviceMemoryTypes();
	auto hostMem = dev.hostMemoryTypes();
	info = vpp::ViewableImageCreateInfo::color(dev, extent, usage,
		{format}).value();
	info.img.imageType = vk::ImageType::e2d;
	info.view.viewType = vk::ImageViewType::e2d;
	info.img.mipLevels = levels;
	info.view.subresourceRange.levelCount = levels;
	vpp::ViewableImage image = {dev, info, devMem};

	auto cb = dev.commandAllocator().get(dev.queueSubmitter().queue().family());
	vk::beginCommandBuffer(cb, {});
	vpp::changeLayout(cb, image.image(),
		vk::ImageLayout::undefined, vk::PipelineStageBits::topOfPipe, {},
		vk::ImageLayout::transferDstOptimal, vk::PipelineStageBits::transfer,
		vk::AccessBits::transferWrite,
		{vk::ImageAspectBits::color, 0, levels, 0, 1});

	// to make sure it doesn't go out of scope
	std::variant<vpp::SubBuffer, vpp::Image> stage;
	if(dataFormat == format) {
		// this is the easy case. we simply upload via buffer
		stage = vpp::fillStaging(cb, image.image(), format,
			vk::ImageLayout::transferDstOptimal, extent, data,
			{vk::ImageAspectBits::color});
	} else {
		// more complicated case. Create linear host image and then
		// blit to real image. Vulkan will do the conversion for us
		auto imgi = info.img;
		imgi.tiling = vk::ImageTiling::linear;
		imgi.usage = vk::ImageUsageBits::transferSrc;
		imgi.format = dataFormat;
		imgi.initialLayout = vk::ImageLayout::preinitialized;
		auto stageImage = vpp::Image(dev, imgi, hostMem);
		vpp::fillMap(stageImage, dataFormat, extent, data,
			{vk::ImageAspectBits::color});

		vpp::changeLayout(cb, stageImage,
			vk::ImageLayout::preinitialized, vk::PipelineStageBits::host, {},
			vk::ImageLayout::transferSrcOptimal, vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

		vk::ImageBlit blit;
		blit.dstSubresource.layerCount = 1;
		blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
		blit.srcSubresource = blit.dstSubresource;
		blit.srcOffsets[1].x = extent.width;
		blit.srcOffsets[1].y = extent.height;
		blit.srcOffsets[1].z = 1u;
		blit.dstOffsets = blit.srcOffsets;
		// nearest filter since both images have the same size.
		vk::cmdBlitImage(cb, stageImage, vk::ImageLayout::transferSrcOptimal,
			image.image(), vk::ImageLayout::transferDstOptimal, {blit},
			vk::Filter::nearest);

		stage = std::move(stageImage);
	}

	// create mipmaps
	if(levels == 1) {
		vpp::changeLayout(cb, image.image(),
			vk::ImageLayout::transferDstOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferWrite,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::allCommands | vk::PipelineStageBits::topOfPipe,
			vk::AccessBits::shaderRead,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});
	} else {
		// bring mip0 into transferSrc layout and set a barrier for initial
		// data transfer to complete
		vpp::changeLayout(cb, image.image(),
			vk::ImageLayout::transferDstOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferWrite,
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

		for(auto i = 1u; i < levels; ++i) {
			// std::max needed for end offsets when the texture is not
			// quadratic: then we would get 0 there although the mipmap
			// still has size 1
			vk::ImageBlit blit;
			blit.srcSubresource.layerCount = 1;
			blit.srcSubresource.mipLevel = i - 1;
			blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.srcOffsets[1].x = std::max(extent.width >> (i - 1), 1u);
			blit.srcOffsets[1].y = std::max(extent.height >> (i - 1), 1u);
			blit.srcOffsets[1].z = 1u;

			blit.dstSubresource.layerCount = 1;
			blit.dstSubresource.mipLevel = i;
			blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.dstOffsets[1].x = std::max(extent.width >> i, 1u);
			blit.dstOffsets[1].y = std::max(extent.height >> i, 1u);
			blit.dstOffsets[1].z = 1u;

			vk::cmdBlitImage(cb, image.image(), vk::ImageLayout::transferSrcOptimal,
				image.image(), vk::ImageLayout::transferDstOptimal, {blit},
				vk::Filter::linear);

			// change layout of current mip level to transferSrc for next
			// mip level
			vpp::changeLayout(cb, image.image(),
				vk::ImageLayout::transferDstOptimal,
				vk::PipelineStageBits::transfer,
				vk::AccessBits::transferWrite,
				vk::ImageLayout::transferSrcOptimal,
				vk::PipelineStageBits::transfer,
				vk::AccessBits::transferRead,
				{vk::ImageAspectBits::color, i, 1, 0, 1});
		}

		// transfer all mip levels to readable layout and set barrier to
		// wait for transfer to complete
		vpp::changeLayout(cb, image.image(),
			vk::ImageLayout::transferSrcOptimal,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			vk::ImageLayout::shaderReadOnlyOptimal,
			vk::PipelineStageBits::allCommands | vk::PipelineStageBits::topOfPipe,
			vk::AccessBits::shaderRead,
			{vk::ImageAspectBits::color, 0, levels, 0, 1});
	}

	vk::endCommandBuffer(cb);

	vk::SubmitInfo submission;
	submission.commandBufferCount = 1;
	submission.pCommandBuffers = &cb.vkHandle();
	dev.queueSubmitter().add(submission);
	dev.queueSubmitter().wait(dev.queueSubmitter().current());

	return image;
}

vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
		nytl::Span<const nytl::StringParam> files, vk::Format format,
		bool cubemap, bool inputSrgb) {
	auto devMem = dev.deviceMemoryTypes();
	auto hostMem = dev.hostMemoryTypes();
	uint32_t layerCount = files.size();
	dlg_assert(layerCount != 0);
	dlg_assert(!cubemap || layerCount == 6u);

	// load data
	std::vector<std::byte*> faceData;
	faceData.reserve(files.size());
	auto width = 0u;
	auto height = 0u;
	bool hdr = isHDR(format);
	vk::Format dataFormat = hdr ?
		vk::Format::r32g32b32a32Sfloat :
		inputSrgb ?  vk::Format::r8g8b8a8Srgb : vk::Format::r8g8b8a8Unorm;

	for(auto filename : files) {
		auto [data, extent] = loadImage(filename, 4, hdr);
		if(!data) {
			dlg_warn("Failed to open texture file {}", filename);

			std::string err = "Could not load image from ";
			err += filename;
			err += ": ";
			err += stbi_failure_reason();
			throw std::runtime_error(err);
		}

		dlg_assert(extent.width > 0 && extent.height > 0);
		if(width == 0 || height == 0) {
			width = extent.width;
			height = extent.height;
		} else if(extent.width != width || extent.height != height) {
			std::string msg = "Images for image array have different sizes:";
			msg += "\n\tFirst image had size (";
			msg += std::to_string(width);
			msg += ",";
			msg += std::to_string(height);
			msg += "), while '" + std::string(filename) + "' has size (";
			msg += std::to_string(extent.width);
			msg += ",";
			msg += std::to_string(extent.height);
			msg += ").";
			throw std::runtime_error(msg);
		}

		faceData.push_back(data);
	}

	// free data at the end of this function
	nytl::ScopeGuard guard([&]{
		for(auto fd : faceData) {
			std::free(fd);
		}
	});

	// create iamge
	auto usage = vk::ImageUsageBits::transferDst | vk::ImageUsageBits::sampled;
	auto imgi = vpp::ViewableImageCreateInfo::color(
		dev, vk::Extent3D {width, height, 1u}, usage,
		{format}).value();

	if(cubemap) {
		imgi.img.flags = vk::ImageCreateBits::cubeCompatible;
		imgi.view.viewType = vk::ImageViewType::cube;
	} else {
		// TODO: vulkan 1.1?
		// imgi.img.flags = vk::ImageCreateBits::e2dArrayCompatble;
		imgi.view.viewType = vk::ImageViewType::e2dArray;
	}
	imgi.img.arrayLayers = files.size();
	imgi.view.subresourceRange.layerCount = files.size();
	vpp::ViewableImage image = {dev, imgi, devMem};

	// upload data
	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});
	vpp::changeLayout(cb, image.image(), vk::ImageLayout::undefined,
		vk::PipelineStageBits::topOfPipe, {}, vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferWrite,
		{vk::ImageAspectBits::color, 0, 1, 0, layerCount});
	std::variant<vpp::SubBuffer, vpp::Image> stage; // keep in scope
	if(format == dataFormat) {
		auto totalSize = files.size() * width * height *
			vpp::formatSize(dataFormat);
		auto stageBuf = vpp::SubBuffer(dev.bufferAllocator(), totalSize,
			vk::BufferUsageBits::transferSrc, 0u, dev.hostMemoryTypes());

		auto map = stageBuf.memoryMap();
		auto span = map.span();
		auto offset = stageBuf.offset();
		std::vector<vk::BufferImageCopy> copies;
		copies.reserve(files.size());
		auto layer = 0u;
		for(auto data : faceData) {
			vk::BufferImageCopy copy {};
			copy.bufferOffset = offset;
			copy.imageExtent = {width, height, 1u};
			copy.imageOffset = {};
			copy.imageSubresource.aspectMask = vk::ImageAspectBits::color;
			copy.imageSubresource.baseArrayLayer = layer++;
			copy.imageSubresource.layerCount = 1u;
			copy.imageSubresource.mipLevel = 0u;
			copies.push_back(copy);
			offset += width * height * 4u;
			doi::write(span, data, width * height * 4u);
		}

		vk::cmdCopyBufferToImage(cb, stageBuf.buffer(), image.image(),
			vk::ImageLayout::transferDstOptimal, copies);
		stage = std::move(stageBuf);
	} else {
		// more complicated case. Create linear host image and then
		// blit to real image. Vulkan will do the conversion for us

		// TODO: better tile, split over width and height. We currently
		// might hit some maxImageWidth limits.
		// Check that; if needed create multiple stage images.
		vk::Extent3D extent {layerCount * width, height, 1u};

		vk::ImageCreateInfo imgi;
		imgi.format = dataFormat;
		imgi.extent = extent;
		imgi.imageType = vk::ImageType::e2d;
		imgi.tiling = vk::ImageTiling::linear;
		imgi.usage = vk::ImageUsageBits::transferSrc;
		imgi.format = dataFormat;
		imgi.initialLayout = vk::ImageLayout::preinitialized;
		auto stageImage = vpp::Image(dev, imgi, hostMem);

		// we map and unmap in each upload iteration below.
		// this will keep the memory map alive (via vpp).
		auto stageMap = stageImage.memoryMap();

		vpp::changeLayout(cb, stageImage,
			vk::ImageLayout::preinitialized, vk::PipelineStageBits::host, {},
			vk::ImageLayout::transferSrcOptimal, vk::PipelineStageBits::transfer,
			vk::AccessBits::transferRead,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

		vk::Offset3D offset;
		std::vector<vk::ImageBlit> blits;
		blits.reserve(layerCount);
		for(auto i = 0u; i < layerCount; ++i) {
			auto data = faceData[i];
			auto dataSize = width * height * vpp::formatSize(dataFormat);
			vpp::fillMap(stageImage, dataFormat, extent, {data, dataSize},
				{vk::ImageAspectBits::color}, offset);

			vk::ImageBlit blit;
			blit.srcSubresource.layerCount = 1;
			blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.dstSubresource.baseArrayLayer = i;
			blit.dstSubresource.layerCount = 1;
			blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.srcOffsets[0] = offset;
			blit.srcOffsets[1].x = offset.x + width;
			blit.srcOffsets[1].y = height;
			blit.srcOffsets[1].z = 1;
			blit.dstOffsets[1].x = width;
			blit.dstOffsets[1].y = height;
			blit.dstOffsets[1].z = 1;
			blits.push_back(blit);

			offset.x += width;
		}

		// nearest filter since images have the same size
		vk::cmdBlitImage(cb, stageImage, vk::ImageLayout::transferSrcOptimal,
			image.image(), vk::ImageLayout::transferDstOptimal, blits,
			vk::Filter::nearest);

		stage = std::move(stageImage);
	}

	vpp::changeLayout(cb, image.image(), vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferWrite,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::allCommands | vk::PipelineStageBits::topOfPipe,
		vk::AccessBits::shaderRead,
		{vk::ImageAspectBits::color, 0, 1, 0, layerCount});
	vk::endCommandBuffer(cb);

	vk::SubmitInfo si {};
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb.vkHandle();
	qs.wait(qs.add(si));

	return image;
}

vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
		nytl::Span<const nytl::StringParam> files, bool cubemap,
		bool srgb, bool hdr) {
	auto format = hdr ?
		vk::Format::r32g32b32a32Sfloat :
		srgb ? vk::Format::r8g8b8a8Srgb : vk::Format::r8g8b8a8Unorm;
	return loadTextureArray(dev, files, format, cubemap, srgb);
}

bool isHDR(vk::Format format) {
	// TODO: not sure about scaled formats, what are those?
	//  also what about packed formats? e.g. vk::Format::b10g11r11UfloatPack32?
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


} // namespace doi
