#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/types.hpp>
#include <tkn/image.hpp>

#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/debug.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/queue.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>

// TODO: support for compressed formats, low priority for now
// TODO: check if blit is supported in format flags
// TODO: currently no support for cubemap arrays

namespace tkn {
using namespace tkn::types;

const vk::ImageUsageFlags TextureCreateParams::defaultUsage =
	vk::ImageUsageBits::sampled |
	vk::ImageUsageBits::transferDst |
	vk::ImageUsageBits::inputAttachment;
const vk::Format TextureCreateParams::defaultFormat = vk::Format::r8g8b8a8Srgb;

Texture::Texture(const vpp::Device& dev, std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& params) {
	*this = {WorkBatcher::createDefault(dev), std::move(img), params};
}

Texture::Texture(const WorkBatcher& wb, std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& params) {
	auto& qs = wb.dev.queueSubmitter();
	auto cb = wb.dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});

	auto cwb = wb;
	cwb.cb = cb;

	InitData data;
	*this = {data, cwb, std::move(img), params};
	init(data, cwb);

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));
}

Texture::Texture(InitData& data, const WorkBatcher& wb,
		std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& params) {
	data = {};
	auto layers = img->layers();
	auto faces = img->faces();
	if(params.cubemap) {
		dlg_assert(faces == 6);
		dlg_assert(params.view.layerCount == 0 || params.view.layerCount == 6);
		dlg_assertm(layers == 1, "cubemap arrays not supported");
		layers *= 6u;
	}

	auto size = img->size();
	auto dataFormat = img->format();
	if(params.srgb && isSRGB(dataFormat) != *params.srgb) {
		dlg_debug("toggling srgb");
		dataFormat = toggleSRGB(dataFormat);
	}

	auto blit = dataFormat != params.format;
	vk::FormatFeatureFlags features {};
	if(blit) {
		features |= vk::FormatFeatureBits::blitDst;
	}

	auto levels = 1u;
	if(!params.mipLevels) {
		levels = img->mipLevels();
	} else {
		if(*params.mipLevels == 0) {
			levels = vpp::mipmapLevels({size.x, size.y});
		} else {
			levels = *params.mipLevels;
		}
	}

	data.fillLevels = 1u;
	data.genLevels = false;
	if(!params.fillMipmaps) {
		data.fillLevels = std::min(img->mipLevels(), levels);
	} else if(*params.fillMipmaps) {
		data.fillLevels = std::min(img->mipLevels(), levels);
		if(data.fillLevels < levels) {
			data.genLevels = true;
		}
	}

	auto usage = params.usage | vk::ImageUsageBits::transferDst;
	if(levels > 1) {
		usage |= vk::ImageUsageBits::transferSrc;
	}

	auto info = vpp::ViewableImageCreateInfo(
		params.format, vk::ImageAspectBits::color, {size.x, size.y},
		usage, vk::ImageTiling::optimal, levels);
	info.img.arrayLayers = layers;
	if(params.cubemap) {
		info.img.flags = vk::ImageCreateBits::cubeCompatible;
		data.cubemap = true;
	}

	dlg_assert(vpp::supported(wb.dev, info.img, features));
	auto devBits = wb.dev.deviceMemoryTypes();
	auto hostBits = wb.dev.hostMemoryTypes();
	image_ = {data.initImage, wb.alloc.memDevice, info.img, devBits};

	data.view = params.view;
	if(data.view.layerCount == 0) {
		data.view.layerCount = layers;
	}
	if(data.view.levelCount == 0) {
		data.view.levelCount = levels;
	}

	data.layers = layers;
	data.dstFormat = params.format;
	data.levels = levels;
	data.image = std::move(img);
	data.dataFormat = dataFormat;
	if(blit) {
		// TODO: when there are multiple pre-gen mipmaps or
		// multiple faces/layers we might not be able to create
		// this linear image on host visible memory. See the vulkan spec,
		// support for linear images is limited.
		// A solution would be to first write the data into an buffer,
		// then copy if to an image, then blit it to the destination...
		auto usage = vk::ImageUsageBits::transferSrc;
		auto info = vpp::ViewableImageCreateInfo(dataFormat,
			vk::ImageAspectBits::color, {size.x, size.y},
			usage, vk::ImageTiling::linear, data.fillLevels);
		info.img.arrayLayers = layers;
		info.img.initialLayout = vk::ImageLayout::preinitialized;
		dlg_assert(vpp::supported(wb.dev, info.img));
		data.stageImage = {data.initStageImage, wb.alloc.memStage, info.img,
			hostBits};
		vpp::nameHandle(data.stageImage, "Texture:stageImage");
	} else {
		auto fmtSize = vpp::formatSize(dataFormat);
		auto dataSize = layers * fmtSize * size.x * size.y;
		for(auto i = 1u; i < data.fillLevels; ++i) {
			auto isize = size;
			isize.x = std::max(isize.x >> i, 1u);
			isize.y = std::max(isize.y >> i, 1u);
			dataSize += layers * fmtSize * isize.x * isize.y;
		}

		auto align = fmtSize;
		auto usage = vk::BufferUsageBits::transferSrc;
		data.stageBuf = {data.initStageBuf, wb.alloc.bufStage, dataSize,
			usage, hostBits, align};
	}
}

void Texture::init(InitData& data, const WorkBatcher& wb) {
	u32 layerCount = data.layers;
	u32 levelCount = data.levels;

	vk::ImageViewCreateInfo ivi;
	ivi.format = data.dstFormat;
	ivi.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	ivi.subresourceRange.baseArrayLayer = data.view.baseArrayLayer;
	ivi.subresourceRange.baseMipLevel = data.view.baseMipLevel;
	ivi.subresourceRange.layerCount = data.view.layerCount;
	ivi.subresourceRange.levelCount = data.view.levelCount;
	if(data.cubemap) {
		ivi.viewType = vk::ImageViewType::cube;
	} else if(ivi.subresourceRange.layerCount > 1) {
		ivi.viewType = vk::ImageViewType::e2dArray;
	} else {
		ivi.viewType = vk::ImageViewType::e2d;
	}

	image_.init(data.initImage, ivi);

	// upload
	auto dataFormat = data.dataFormat;
	auto size = data.image->size();
	u32 width = size.x;
	u32 height = size.y;
	auto cb = wb.cb;

	// Bring the dst image into transferDstOptimal layout for initial write
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

	auto blit = data.dstFormat != dataFormat;
	if(blit) {
		dlg_assert(data.stageImage);
		data.stageImage.init(data.initStageImage);

		std::vector<vk::ImageBlit> blits;
		blits.reserve(data.fillLevels);
		for(auto m = 0u; m < data.fillLevels; ++m) {
			auto mwidth = std::max(width >> m, 1u);
			auto mheight = std::max(height >> m, 1u);
			auto lsize = mwidth * mheight * vpp::formatSize(dataFormat);

			for(auto l = 0u; l < layerCount; ++l) {
				// TODO: could offer optimization in case the
				// data is subresource layout is tightly packed:
				// directly let provider write to map
				auto layer = data.cubemap ? 0u : l;
				auto face = data.cubemap ? l : 0u;
				auto span = data.image->read(m, layer, face);
				dlg_assertm(span.size() == lsize, "{} vs {}",
					span.size(), lsize);

				vk::ImageSubresource subres;
				subres.arrayLayer = l;
				subres.mipLevel = m;
				subres.aspectMask = vk::ImageAspectBits::color;
				vpp::fillMap(data.stageImage, dataFormat,
					{mwidth, mheight, 1u}, span, subres);
			}

			vk::ImageBlit blit;
			blit.srcSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.srcSubresource.layerCount = layerCount;
			blit.srcSubresource.mipLevel = m;
			blit.dstSubresource.aspectMask = vk::ImageAspectBits::color;
			blit.dstSubresource.layerCount = layerCount;
			blit.dstSubresource.mipLevel = m;
			blit.srcOffsets[1].x = mwidth;
			blit.srcOffsets[1].y = mheight;
			blit.srcOffsets[1].z = 1;
			blit.dstOffsets[1].x = mwidth;
			blit.dstOffsets[1].y = mheight;
			blit.dstOffsets[1].z = 1;
			blits.push_back(blit);
		}

		// bring stage image into transferSrcOptimal layout for blit
		barrier.image = data.stageImage;
		barrier.oldLayout = vk::ImageLayout::preinitialized;
		barrier.srcAccessMask = vk::AccessBits::hostWrite;
		barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		barrier.subresourceRange =
			{vk::ImageAspectBits::color, 0, 1, 0, layerCount};
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::host,
			vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

		// nearest is enough, we are not scaling in any way
		vk::cmdBlitImage(cb, data.stageImage,
			vk::ImageLayout::transferSrcOptimal, image_.image(),
			vk::ImageLayout::transferDstOptimal, blits,
			vk::Filter::nearest);
	} else {
		data.stageBuf.init(data.initStageBuf);
		dlg_assert(data.stageBuf.buffer());

		std::vector<vk::BufferImageCopy> copies;
		copies.reserve(layerCount);

		auto map = data.stageBuf.memoryMap();
		auto mapSpan = map.span();
		auto offset = data.stageBuf.offset();
		for(auto m = 0u; m < data.fillLevels; ++m) {
			auto mwidth = std::max(width >> m, 1u);
			auto mheight = std::max(height >> m, 1u);
			auto lsize = mwidth * mheight * vpp::formatSize(dataFormat);

			for(auto l = 0u; l < layerCount; ++l) {
				auto layer = data.cubemap ? 0u : l;
				auto face = data.cubemap ? l : 0u;

				auto layerSpan = mapSpan.first(lsize);
				if(!data.image->read(layerSpan, m, layer, face)) {
					throw std::runtime_error("Image reading failed");
				}

				mapSpan = mapSpan.last(mapSpan.size() - lsize);
				vk::BufferImageCopy copy {};
				copy.bufferOffset = offset;
				copy.imageExtent = {mwidth, mheight, 1};
				copy.imageOffset = {};
				copy.imageSubresource.aspectMask = vk::ImageAspectBits::color;
				copy.imageSubresource.baseArrayLayer = l;
				copy.imageSubresource.layerCount = 1u;
				copy.imageSubresource.mipLevel = m;
				copies.push_back(copy);
				offset += lsize;
			}
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
	if(data.genLevels) {
		// bring all already filled levels into transferSrc layout and set a
		// barrier for initial data transfer to complete
		barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
		barrier.dstAccessMask = vk::AccessBits::transferRead;
		barrier.subresourceRange.levelCount = data.fillLevels;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::transfer,
			{}, {}, {}, {{barrier}});

		for(auto i = data.fillLevels; i < data.levels; ++i) {
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

		// bring all levels back to shaderReadOnly layout
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
		case vk::Format::b8g8r8a8Srgb:
			return true;
		default:
			return false;
	}
}

vk::Format toggleSRGB(vk::Format format) {
	switch(format) {
		case vk::Format::r8Srgb:
			return vk::Format::r8Unorm;
		case vk::Format::r8g8Srgb:
			return vk::Format::r8g8Unorm;
		case vk::Format::r8g8b8Srgb:
			return vk::Format::r8g8b8Unorm;
		case vk::Format::r8g8b8a8Srgb:
			return vk::Format::r8g8b8a8Unorm;
		case vk::Format::b8g8r8a8Srgb:
			return vk::Format::b8g8r8a8Unorm;

		case vk::Format::r8Unorm:
			return vk::Format::r8Srgb;
		case vk::Format::r8g8Unorm:
			return vk::Format::r8g8Srgb;
		case vk::Format::r8g8b8Unorm:
			return vk::Format::r8g8b8Srgb;
		case vk::Format::r8g8b8a8Unorm:
			return vk::Format::r8g8b8a8Srgb;
		case vk::Format::b8g8r8a8Unorm:
			return vk::Format::b8g8r8a8Srgb;

		default: return format;
	}
}

} // namespace tkn
