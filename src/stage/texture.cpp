#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <vpp/vk.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/queue.hpp>
#include <vpp/formats.hpp>
#include <dlg/dlg.hpp>
#include <stdexcept>
#include <nytl/scope.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace doi {

std::tuple<const std::byte*, vk::Extent3D> loadImage(nytl::StringParam filename,
		unsigned channels = 4, bool hdr = false) {
	if(stbi_is_hdr(filename.c_str()) != hdr) {
		dlg_warn("stbi hdr conversion");
	}

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
	auto ptr = reinterpret_cast<const std::byte*>(data);
	return {ptr, extent};
}

vpp::ViewableImage loadTexture(const vpp::Device& dev,
		nytl::StringParam filename, bool hdr) {
	auto [data, extent] = loadImage(filename, 4, hdr);
	auto format = hdr ?
		vk::Format::r32g32b32a32Sfloat : // TODO: support 16 bit float
		vk::Format::r8g8b8a8Srgb;
	auto dataSize = extent.width * extent.height * vpp::formatSize(format);
	auto dspan = nytl::Span<const std::byte>(data, dataSize);
	auto img = loadTexture(dev, extent, format, dspan);
	::free(const_cast<std::byte*>(data));
	return img;
}

vpp::ViewableImage loadTextureArray(const vpp::Device& dev,
		nytl::Span<const nytl::StringParam> files, bool cubemap,
		bool hdr) {
	uint32_t layerCount = files.size();
	dlg_assert(layerCount != 0);
	dlg_assert(!cubemap || layerCount == 6u);

	// load data
	std::vector<const std::byte*> faceData;
	faceData.reserve(files.size());
	auto format = hdr ?
		vk::Format::r32g32b32a32Sfloat : // TODO: support 16 bit float
		vk::Format::r8g8b8a8Srgb;
	auto width = 0u;
	auto height = 0u;

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

		faceData.push_back(reinterpret_cast<const std::byte*>(data));
	}

	// free data at the end of this function
	nytl::ScopeGuard guard([&]{
		for(auto fd : faceData) {
			std::free(const_cast<std::byte*>(fd));
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
	vpp::ViewableImage image = {dev, imgi};

	// upload data
	auto totalSize = files.size() * width * height * vpp::formatSize(format);
	auto stage = vpp::SubBuffer(dev.bufferAllocator(), totalSize,
		vk::BufferUsageBits::transferSrc, 0u, dev.hostMemoryTypes());

	auto map = stage.memoryMap();
	auto span = map.span();
	auto offset = stage.offset();
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

	map = {}; // unmap
	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());

	// TODO: we can't really know the stages and bits of the dependencies
	vk::beginCommandBuffer(cb, {});
	vpp::changeLayout(cb, image.image(), vk::ImageLayout::undefined,
		vk::PipelineStageBits::topOfPipe, {}, vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferWrite,
		{vk::ImageAspectBits::color, 0, 1, 0, layerCount});
	vk::cmdCopyBufferToImage(cb, stage.buffer(), image.image(),
		vk::ImageLayout::transferDstOptimal, copies);
	vpp::changeLayout(cb, image.image(), vk::ImageLayout::transferDstOptimal,
		vk::PipelineStageBits::transfer, vk::AccessBits::transferWrite,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::fragmentShader,
		vk::AccessBits::shaderRead,
		{vk::ImageAspectBits::color, 0, 1, 0, layerCount});
	vk::endCommandBuffer(cb);

	vk::SubmitInfo si {};
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb.vkHandle();
	qs.wait(qs.add(si));

	return image;
}

vpp::ViewableImage loadTexture(const vpp::Device& dev, vk::Extent3D extent,
		vk::Format format, nytl::Span<const std::byte> data) {
	vpp::ViewableImageCreateInfo info;
	auto usage = vk::ImageUsageBits::transferDst | vk::ImageUsageBits::sampled;
	info = vpp::ViewableImageCreateInfo::color(dev, extent, usage,
		{format}).value();

	info.img.imageType = vk::ImageType::e2d;
	info.view.viewType = vk::ImageViewType::e2d;
	auto memBits = dev.memoryTypeBits(vk::MemoryPropertyBits::deviceLocal);

	vpp::ViewableImage image = {dev, info, memBits};

	// TODO: return it as well instead of waiting for it...
	// then also return the staging area
	auto cmdBuf = dev.commandAllocator().get(dev.queueSubmitter().queue().family());
	vk::beginCommandBuffer(cmdBuf, {});
	vpp::changeLayout(cmdBuf, image.image(),
		vk::ImageLayout::undefined, vk::PipelineStageBits::topOfPipe, {},
		vk::ImageLayout::transferDstOptimal, vk::PipelineStageBits::transfer,
		vk::AccessBits::transferWrite,
		{vk::ImageAspectBits::color, 0, 1, 0, 1});

	auto stage = vpp::fillStaging(cmdBuf, image.image(), format,
		vk::ImageLayout::transferDstOptimal, extent, data,
		{vk::ImageAspectBits::color});

	vpp::changeLayout(cmdBuf, image.image(),
		vk::ImageLayout::transferDstOptimal, vk::PipelineStageBits::transfer,
		vk::AccessBits::transferWrite,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::allGraphics,
		vk::AccessBits::shaderRead,
		{vk::ImageAspectBits::color, 0, 1, 0, 1});

	vk::endCommandBuffer(cmdBuf);

	vk::SubmitInfo submission;
	submission.commandBufferCount = 1;
	submission.pCommandBuffers = &cmdBuf.vkHandle();
	dev.queueSubmitter().add(submission);
	dev.queueSubmitter().wait(dev.queueSubmitter().current());

	return image;
}

} // namespace doi
