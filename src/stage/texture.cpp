#include <stage/texture.hpp>
#include <vpp/vk.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/queue.hpp>
#include <vpp/formats.hpp>
#include <dlg/dlg.hpp>
#include <stdexcept>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace doi {

std::tuple<const std::byte*, vk::Extent3D> loadImage(nytl::StringParam filename,
		unsigned channels = 4) {
	int width, height, ch;
	unsigned char* data = stbi_load(filename.c_str(), &width, &height,
		&ch, channels);
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

vpp::ViewableImage loadTexture(vpp::Device& dev, nytl::StringParam filename) {
	auto [data, extent] = loadImage(filename);
	vpp::ViewableImageCreateInfo info;
	info = vpp::ViewableImageCreateInfo::color(dev, extent).value();

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

	auto format = vk::Format::r8g8b8a8Unorm;
	auto dataSize = extent.width * extent.height * 4;
	auto dspan = nytl::Span<const std::byte>(data, dataSize);
	auto stage = vpp::fillStaging(cmdBuf, image.image(), format,
		vk::ImageLayout::transferDstOptimal, extent, dspan,
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
	free(const_cast<std::byte*>(data));

	return image;
}

vpp::ViewableImage loadTextureArray(vpp::Device& dev,
		nytl::Span<const nytl::StringParam> files) {

	uint32_t layerCount = files.size();
	dlg_assert(layerCount != 0);

	auto layer = 0u;
	vk::Extent3D extent;
	vpp::ViewableImage image;

	auto cmdBuf = dev.commandAllocator().get(dev.queueSubmitter().queue().family());
	vk::beginCommandBuffer(cmdBuf, {});
	std::vector<vpp::SubBuffer> stages;

	for(auto file : files) {
		auto [data, iextent] = loadImage(file);
		if(layer == 0u) {
			// create image with extent
			extent = iextent;
			vpp::ViewableImageCreateInfo info;
			info = vpp::ViewableImageCreateInfo::color(dev, extent).value();

			info.img.arrayLayers = layerCount;
			info.img.imageType = vk::ImageType::e2d;
			info.view.viewType = vk::ImageViewType::e2dArray;
			info.view.subresourceRange.layerCount = layerCount;
			auto memBits = dev.memoryTypeBits(vk::MemoryPropertyBits::deviceLocal);

			image = {dev, info, memBits};
			vpp::changeLayout(cmdBuf, image.image(),
				vk::ImageLayout::undefined, vk::PipelineStageBits::topOfPipe, {},
				vk::ImageLayout::transferDstOptimal, vk::PipelineStageBits::transfer,
				vk::AccessBits::transferWrite,
				{vk::ImageAspectBits::color, 0, 1, 0, layerCount});
		} else {
			if(iextent.width != extent.width || iextent.height != extent.height) {
				std::string msg = "Images for image array have different sizes:";
				msg += "\n\tFirst image had size (";
				msg += std::to_string(extent.width);
				msg += ",";
				msg += std::to_string(extent.height);
				msg += "), while '" + std::string(file) + "' has size (";
				msg += std::to_string(iextent.width);
				msg += ",";
				msg += std::to_string(iextent.height);
				msg += ").";
				throw std::runtime_error(msg);
			}
		}

		// write
		auto format = vk::Format::r8g8b8a8Unorm;
		auto dataSize = extent.width * extent.height * 4;
		auto dspan = nytl::Span<const std::byte>(data, dataSize);
		stages.push_back(vpp::fillStaging(cmdBuf, image.image(), format,
			vk::ImageLayout::transferDstOptimal, extent, dspan,
			{vk::ImageAspectBits::color, 0, layer}));

		free(const_cast<std::byte*>(data));
		++layer;
	}

	vpp::changeLayout(cmdBuf, image.image(),
		vk::ImageLayout::transferDstOptimal, vk::PipelineStageBits::transfer,
		vk::AccessBits::transferWrite,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::PipelineStageBits::allGraphics,
		vk::AccessBits::shaderRead,
		{vk::ImageAspectBits::color, 0, 1, 0, layerCount});
	vk::endCommandBuffer(cmdBuf);

	vk::SubmitInfo submission;
	submission.commandBufferCount = 1;
	submission.pCommandBuffers = &cmdBuf.vkHandle();
	dev.queueSubmitter().add(submission);
	dev.queueSubmitter().wait(dev.queueSubmitter().current());

	return image;
}

} // namespace doi
