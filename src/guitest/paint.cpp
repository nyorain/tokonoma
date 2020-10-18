#include "paint.hpp"
#include <dlg/dlg.hpp>
#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC // needed, otherwise we mess with other usages
#include "stb_image.h"
#pragma GCC diagnostic pop

namespace rvg2 {

PaintData colorPaint(const Vec4f& color) {
	PaintData ret;
	ret.transform = nytl::identity<4, float>();
	ret.transform[3][3] = float(PaintType::color);
	ret.inner = color;
	return ret;
}

PaintData linearGradient(Vec2f start, Vec2f end,
		const Vec4f& startColor, const Vec4f& endColor) {
	PaintData ret;
	ret.transform = nytl::identity<4, float>();

	ret.transform[3][3] = float(PaintType::linGrad);
	ret.inner = startColor;
	ret.outer = endColor;
	ret.custom = {start.x, start.y, end.x, end.y};

	return ret;
}

PaintData radialGradient(Vec2f center, float innerRadius, float outerRadius,
		const Vec4f& innerColor, const Vec4f& outerColor) {
	PaintData ret;
	ret.transform = nytl::identity<4, float>();
	ret.transform[3][3] = float(PaintType::radGrad);

	ret.inner = innerColor;
	ret.outer = outerColor;
	ret.custom = {center.x, center.y, innerRadius, outerRadius};

	return ret;
}

PaintData texturePaintRGBA(const nytl::Mat4f& transform) {
	PaintData ret;
	ret.transform = transform;
	ret.transform[3][3] = float(PaintType::textureRGBA);
	ret.inner = {1.f, 1.f, 1.f, 1.f};
	return ret;
}

PaintData texturePaintA(const nytl::Mat4f& transform) {
	PaintData ret;
	ret.transform = transform;
	ret.transform[3][3] = float(PaintType::textureA);
	ret.inner = {1.f, 1.f, 1.f, 1.f};
	return ret;
}

PaintData pointColorPaint() {
	PaintData ret;
	ret.transform = nytl::identity<4, float>();
	ret.transform[3][3] = float(PaintType::pointColor);
	return ret;
}

// Texture
Texture::Texture(UpdateContext& ctx, nytl::StringParam filename, Type type) {
	init(ctx, filename, type);
}

Texture::Texture(UpdateContext& ctx, Vec2ui size, nytl::Span<const std::byte> data,
		Type type) {
	init(ctx, size, data, type);
}

void Texture::init(UpdateContext& ctx, nytl::StringParam filename, Type type) {
	context_ = &ctx;
	type_ = type;

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

	if((channels == 1 || channels == 3) && type == TextureType::a8) {
		dlg_warn("Creating a8 texture from alpha-less image");
	}

	dlg_assert(width > 0 && height > 0);
	std::vector<std::byte> alphaData;
	auto ptr = reinterpret_cast<const std::byte*>(data);
	size_t dataSize = width * height * 4u;
	if(type == TextureType::a8) {
		alphaData.resize(width * height);
		ptr = alphaData.data();
		dataSize /= 4;
		for(auto i = 0u; i < unsigned(width * height); ++i) {
			alphaData[i] = std::byte {data[4 * i + 3]};
		}
	}

	size_.x = width;
	size_.y = height;
	create();
	upload({ptr, ptr + dataSize}, vk::ImageLayout::undefined);
	free(data);
}

void Texture::init(UpdateContext& ctx, Vec2ui size,
		nytl::Span<const std::byte> data, Type type) {
	context_ = &ctx;
	size_ = size;
	type_ = type;

	create();
	upload(data, vk::ImageLayout::undefined);
}

void Texture::create() {
	constexpr auto usage =
		vk::ImageUsageBits::transferDst |
		vk::ImageUsageBits::sampled;

	vk::Extent3D extent;
	extent.width = size_.x;
	extent.height = size_.y;
	extent.depth = 1u;

	vpp::ViewableImageCreateInfo info(vk::Format::r8g8b8a8Srgb,
		vk::ImageAspectBits::color, {size_.x, size_.y},
		usage);

	if(type() == TextureType::a8) {
		info.img.format = vk::Format::r8Unorm;
		info.view.format = vk::Format::r8Unorm;
		info.view.components = {
			vk::ComponentSwizzle::zero,
			vk::ComponentSwizzle::zero,
			vk::ComponentSwizzle::zero,
			vk::ComponentSwizzle::r,
		};
	}

	auto& dev = context().device();
	auto memBits = dev.memoryTypeBits(vk::MemoryPropertyBits::deviceLocal);

	image_ = {updateContext().devMemAllocator(), info, memBits};
}

void Texture::upload(nytl::Span<const std::byte> data, vk::ImageLayout layout) {
	auto cmdBuf = updateContext().recordableUploadCmdBuf();

	vk::ImageMemoryBarrier barrier;
	barrier.image = image_.image();
	barrier.oldLayout = layout;
	barrier.newLayout = vk::ImageLayout::transferDstOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferWrite;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
	vk::cmdPipelineBarrier(cmdBuf, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});
	layout = vk::ImageLayout::transferDstOptimal;

	auto size = vk::Extent3D{size_.x, size_.y, 1u};
	auto format = type_ == Type::a8 ?
		vk::Format::r8Unorm : // TODO: use srgb here as well?
		vk::Format::r8g8b8a8Srgb;
	auto stage = vpp::fillStaging(cmdBuf, image_.image(), format, layout,
		size, data, {vk::ImageAspectBits::color});

	barrier.oldLayout = layout;
	barrier.srcAccessMask = vk::AccessBits::transferWrite;
	barrier.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	barrier.dstAccessMask = vk::AccessBits::shaderRead;
	vk::cmdPipelineBarrier(cmdBuf, vk::PipelineStageBits::transfer,
		vk::PipelineStageBits::allGraphics, {}, {}, {}, {{barrier}});

	updateContext().keepAlive(std::move(stage));
}

void Texture::update(std::vector<std::byte> data) {
	dlg_assert(size_.x * size_.y * (type_ == Type::a8 ? 1u : 4u) == data.size());
	pending_ = std::move(data);
	registerDeviceUpdate();
}

UpdateFlags Texture::updateDevice(std::vector<std::byte> data) {
	dlg_assert(size_.x * size_.y * (type_ == Type::a8 ? 1u : 4u) == data.size());
	pending_ = std::move(data);
	return updateDevice();
}

UpdateFlags Texture::updateDevice() {
	upload(pending_, vk::ImageLayout::shaderReadOnlyOptimal);
	return UpdateFlags::none;
}

} // namespace rvg2
