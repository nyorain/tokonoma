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

vpp::ViewableImage loadTexture(vpp::Device& dev, nytl::StringParam filename) {

	int width, height, channels;
	unsigned char* data = stbi_load(filename.c_str(), &width, &height,
		&channels, 4);
	if(!data) {
		dlg_warn("Failed to open texture file {}", filename);

		std::string err = "Could not load image from ";
		err += filename;
		err += ": ";
		err += stbi_failure_reason();
		throw std::runtime_error(err);
	}

	dlg_assert(width > 0 && height > 0);
	std::vector<std::byte> alphaData;
	auto ptr = reinterpret_cast<const std::byte*>(data);
	size_t dataSize = width * height * 4u;

	vk::Extent3D extent;
	extent.width = width;
	extent.height = height;
	extent.depth = 1u;

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

	auto uwidth = unsigned(width), uheight = unsigned(height);
	auto size = vk::Extent3D {uwidth, uheight, 1u};
	auto format = vk::Format::r8g8b8a8Unorm;
	auto dspan = nytl::Span<const std::byte>(ptr, dataSize);
	auto stage = vpp::fillStaging(cmdBuf, image.image(), format,
		vk::ImageLayout::transferDstOptimal, size, dspan,
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
	free(data);

	return image;
}

} // namespace doi
