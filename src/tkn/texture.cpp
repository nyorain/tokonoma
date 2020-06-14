#include <tkn/texture.hpp>
#include <tkn/bits.hpp>
#include <tkn/types.hpp>
#include <tkn/image.hpp>
#include <tkn/formats.hpp>

#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/debug.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/queue.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>

// TODO: support for compressed formats, low priority for now
// TODO: check if blit is supported in format flags

namespace tkn {
using namespace tkn::types;

const vk::ImageUsageFlags TextureCreateParams::defaultUsage =
	vk::ImageUsageBits::sampled |
	vk::ImageUsageBits::transferDst |
	vk::ImageUsageBits::inputAttachment;

/*
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
	auto cubemap = params.cubemap ? *params.cubemap : img->cubemap();
	if(cubemap) {
		dlg_assertm(layers % 6 == 0, "A cubemap image must have 6 faces");
		dlg_assertm(params.view.layerCount % 6u == 0u, "A cubemap image view "
			"must have 6 layers (or a multiple of 6 for cubemap arrays)");
	}

	auto size = img->size();
	auto dataFormat = img->format();
	if(params.srgb && isSRGB(dataFormat) != *params.srgb) {
		dlg_debug("toggling srgb");
		dataFormat = toggleSRGB(dataFormat);
	}

	auto dstFormat = params.format ? *params.format : dataFormat;
	auto blit = dataFormat != dstFormat;
	vk::FormatFeatureFlags features {};
	if(blit) {
		features |= vk::FormatFeatureBits::blitDst;
	}

	auto levels = 1u;
	if(!params.mipLevels) {
		levels = img->mipLevels();
	} else {
		if(*params.mipLevels == 0) {
			levels = vpp::mipmapLevels({size.x, size.y, size.z});
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

	vk::ImageCreateInfo ici;
	ici.arrayLayers = layers;
	ici.extent = {size.x, size.y, size.z};
	ici.mipLevels = levels;
	ici.format = dstFormat;
	ici.usage = usage;
	ici.initialLayout = vk::ImageLayout::undefined;
	ici.imageType = minImageType(ici.extent);
	ici.tiling = vk::ImageTiling::optimal;
	ici.samples = vk::SampleCountBits::e1;

	if(cubemap) {
		ici.flags = vk::ImageCreateBits::cubeCompatible;
		data.cubemap = true;
	}

	if(!vpp::supported(wb.dev, ici, features)) {
		throw std::runtime_error("Image parameters not supported by device");
	}

	auto devBits = wb.dev.deviceMemoryTypes();
	auto hostBits = wb.dev.hostMemoryTypes();
	image_ = {data.initImage, wb.alloc.memDevice, ici, devBits};

	data.view = params.view;
	if(data.view.layerCount == 0) {
		data.view.layerCount = layers;
	}
	if(data.view.levelCount == 0) {
		data.view.levelCount = levels;
	}

	data.layers = layers;
	data.dstFormat = dstFormat;
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
		auto dataSize = layers * fmtSize * size.x * size.y * size.z;
		for(auto i = 1u; i < data.fillLevels; ++i) {
			auto isize = size;
			isize.x = std::max(size.x >> i, 1u);
			isize.y = std::max(size.y >> i, 1u);
			isize.z = std::max(size.z >> i, 1u);
			dataSize += layers * fmtSize * isize.x * isize.y * isize.z;
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
		if(ivi.subresourceRange.layerCount > 6u) {
			ivi.viewType = vk::ImageViewType::cubeArray;
		} else {
			ivi.viewType = vk::ImageViewType::cube;
		}
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
	u32 depth = size.z;
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
			auto mdepth = std::max(depth >> m, 1u);
			auto lsize = mwidth * mheight * mdepth * vpp::formatSize(dataFormat);

			for(auto l = 0u; l < layerCount; ++l) {
				// TODO: could offer optimization in case the
				// data is subresource layout is tightly packed:
				// directly let provider write to map
				auto span = data.image->read(m, l);
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
			auto mdepth = std::max(depth >> m, 1u);
			auto lsize = mwidth * mheight * mdepth * vpp::formatSize(dataFormat);

			for(auto l = 0u; l < layerCount; ++l) {
				auto layerSpan = mapSpan.first(lsize);
				if(!data.image->read(layerSpan, m, l)) {
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
	// TODO: use tkn::downscale here
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
		vk::PipelineStageBits::allCommands | vk::PipelineStageBits::bottomOfPipe,
		{}, {}, {}, {{barrier}});
}
*/

FillData createFill(WorkBatcher& wb, const vpp::Image& img, vk::Format dstFormat,
		std::unique_ptr<ImageProvider> provider, unsigned maxNumLevels,
		std::optional<bool> forceSRGB) {

	auto& dev = img.device();

	FillData res;
	res.dstFormat = dstFormat;
	res.target = img;
	res.source = std::move(provider);

	auto hostBits = dev.hostMemoryTypes();
	auto devBits = dev.deviceMemoryTypes();

	auto size = res.source->size();
	auto fillLevelCount = std::min(res.source->mipLevels(), maxNumLevels);
	auto layerCount = res.source->layers();
	auto srcFormat = res.source->format();
	res.numLevels = maxNumLevels;

	if(forceSRGB && isSRGB(srcFormat) != *forceSRGB) {
		dlg_debug("toggling srgb");
		srcFormat = toggleSRGB(srcFormat);
	}

	res.srcFormat = srcFormat;
	auto blit = res.srcFormat != dstFormat;
	if(blit) {
		vk::ImageCreateInfo ici;
		ici.arrayLayers = layerCount;
		ici.extent = {size.x, size.y, size.z};
		ici.usage = vk::ImageUsageBits::transferSrc;
		ici.format = srcFormat;
		ici.tiling = vk::ImageTiling::linear;
		ici.mipLevels = fillLevelCount;
		ici.initialLayout = vk::ImageLayout::preinitialized;
		ici.imageType = minImageType(ici.extent);
		ici.samples = vk::SampleCountBits::e1;

		// when there are multiple pre-gen mipmaps or
		// multiple faces/layers or size.z > 1, we might not be able to create
		// this linear image on host visible memory. See the vulkan spec,
		// support for linear images is limited.
		// The solution is to first write the data into an buffer,
		// then copy if to an image, then blit it to the destination...
		auto features = vk::FormatFeatureBits::blitSrc;
		auto memAlloc = &wb.alloc.memHost;
		auto memBits = hostBits;
		if(!vpp::supported(dev, ici, features)) {
			memAlloc = &wb.alloc.memDevice;
			memBits = devBits;

			ici.initialLayout = vk::ImageLayout::undefined;
			ici.tiling = vk::ImageTiling::optimal;
			ici.usage |= vk::ImageUsageBits::transferDst;

			// this might happen indeed, e.g. if the format just
			// isn't supported, or at least not supported for blitting.
			// We have no other choice but to do the conversion on the cpu
			// in that case.
			if(!vpp::supported(dev, ici, features)) {
				blit = false;
				res.cpuConversion = true;
				dlg_debug("createFill: need to perform cpu format conversion");
			} else {
				dlg_debug("createFill: need stageBuffer -> stageImage -> image chain");
				res.copyToStageImage = true;
			}
		}

		if(blit) {
			res.stageImage = {res.initStageImage, *memAlloc, ici, memBits};
			vpp::nameHandle(res.stageImage, "Texture:stageImage");
		}
	}

	if(!blit || res.copyToStageImage) {
		auto fmtSize = vpp::formatSize(res.cpuConversion ? dstFormat : srcFormat);
		auto dataSize = layerCount * fmtSize * size.x * size.y * size.z;
		for(auto i = 1u; i < fillLevelCount; ++i) {
			auto isize = size;
			isize.x = std::max(size.x >> i, 1u);
			isize.y = std::max(size.y >> i, 1u);
			isize.z = std::max(size.z >> i, 1u);
			dataSize += layerCount * fmtSize * isize.x * isize.y * isize.z;
		}

		auto align = std::max<vk::DeviceSize>(fmtSize,
			dev.properties().limits.optimalBufferCopyOffsetAlignment);
		auto usage = vk::BufferUsageBits::transferSrc;
		res.stageBuffer = {res.initStageBuffer, wb.alloc.bufHost,
			dataSize, usage, hostBits, align};
	}

	return res;
}

void doFill(FillData& data, vk::CommandBuffer cb) {
	dlg_assert(data.target);
	dlg_assert(data.source);

	auto levelCount = data.numLevels;
	auto numFillLevels = std::min(levelCount, data.source->mipLevels());
	auto layerCount = data.source->layers();
	auto [width, height, depth] = data.source->size();

	vk::ImageMemoryBarrier barriers[2];
	auto& b0 = barriers[0];
	auto& b1 = barriers[1];

	b0.image = data.target;
	// TODO: when we support filling in just a part of the image,
	// we have to change this and know the actual current layout.
	// Transitioning from ImageLayout::undefined always works but might
	// discard the contents
	b0.oldLayout = vk::ImageLayout::undefined;
	b0.srcAccessMask = {};
	b0.newLayout = vk::ImageLayout::transferDstOptimal;
	b0.dstAccessMask = vk::AccessBits::transferWrite;
	b0.subresourceRange = {vk::ImageAspectBits::color, 0, numFillLevels, 0, layerCount};

	auto bcount = 1u;
	if(data.copyToStageImage) {
		b1 = b0;
		b1.image = data.stageImage;
		++bcount;
	}

	vk::cmdPipelineBarrier(cb,
		vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::transfer, {}, {}, {}, {barriers, bcount});

	auto blit = (data.dstFormat != data.srcFormat) && !data.cpuConversion;
	if(!blit || data.copyToStageImage) {
		data.stageBuffer.init(data.initStageBuffer);
		dlg_assert(data.stageBuffer.size());

		std::vector<vk::BufferImageCopy> copies;
		copies.reserve(numFillLevels);

		auto map = data.stageBuffer.memoryMap();
		auto mapSpan = map.span();
		auto offset = data.stageBuffer.offset();
		for(auto m = 0u; m < numFillLevels; ++m) {
			auto mwidth = std::max(width >> m, 1u);
			auto mheight = std::max(height >> m, 1u);
			auto mdepth = std::max(depth >> m, 1u);
			auto lsize = mwidth * mheight * mdepth *
				vpp::formatSize(data.cpuConversion ? data.dstFormat : data.srcFormat);

			for(auto l = 0u; l < layerCount; ++l) {
				auto layerSpan = mapSpan.first(lsize);

				if(data.cpuConversion) {
					auto img = data.source->read(m, l);
					auto expected = mwidth * mheight * mdepth * vpp::formatSize(data.srcFormat);
					if(img.size() != expected) {
						dlg_error("Unexpected image read size: {}, expected {}",
							img.size(), expected);
						throw std::runtime_error("Image reading failed");
					}

					// cpu conversion loop
					for(auto d = 0u; d < mdepth; ++d) {
						for(auto y = 0u; y < mheight; ++y) {
							for(auto x = 0u; x < mwidth; ++x) {
								convert(data.dstFormat, layerSpan, data.srcFormat, img);
							}
						}
					}

					dlg_assert(img.empty());
					dlg_assert(layerSpan.empty());
				} else {
					auto res = data.source->read(layerSpan, m, l);
					if(res != lsize) {
						dlg_error("Unexpected image read size: {}, expected {}", res, lsize);
						throw std::runtime_error("Image reading failed");
					}
				}

				mapSpan = mapSpan.last(mapSpan.size() - lsize);
			}

			vk::BufferImageCopy copy {};
			copy.bufferOffset = offset;
			copy.imageExtent = {mwidth, mheight, mdepth};
			copy.imageOffset = {};
			copy.imageSubresource.aspectMask = vk::ImageAspectBits::color;
			copy.imageSubresource.baseArrayLayer = 0u;
			copy.imageSubresource.layerCount = layerCount;
			copy.imageSubresource.mipLevel = m;
			copies.push_back(copy);
			offset += lsize * layerCount;
		}

		if(!map.coherent()) {
			map.flush();
		}

		auto target0 = data.copyToStageImage ?
			data.stageImage.vkHandle() : data.target;
		vk::cmdCopyBufferToImage(cb, data.stageBuffer.buffer(), target0,
			vk::ImageLayout::transferDstOptimal, copies);
	}

	if(blit) {
		dlg_assert(data.stageImage);
		dlg_assert(!data.cpuConversion);
		data.stageImage.init(data.initStageImage);

		// TODO(PERF, low): keep memory map view active over this whole time
		// if we write to stageImage via map.
		// At the moment, each fillMap call will map and unmap the memory
		// (also flush it everytime). This is hard because we might
		// have to fill multiple mips, that each can have a different
		// subresource layout, different offsets etc...

		std::vector<vk::ImageBlit> blits;
		blits.reserve(numFillLevels);
		auto fmtSize = vpp::formatSize(data.srcFormat);
		for(auto m = 0u; m < numFillLevels; ++m) {
			auto mwidth = std::max(width >> m, 1u);
			auto mheight = std::max(height >> m, 1u);
			auto mdepth = std::max(depth >> m, 1u);
			auto lsize = mwidth * mheight * mdepth * fmtSize;

			if(!data.copyToStageImage) {
				// We check if the subresource layout is tight. If so,
				// we can let the image provider directly write into
				// the mapped memory.
				auto sl = vk::getImageSubresourceLayout(data.stageImage.device(),
					data.stageImage, {vk::ImageAspectBits::color, m, 0u});
				if(sl.arrayPitch == lsize) { // tight subresource layout
					auto mapBegin = vpp::texelAddress(sl, fmtSize, 0u, 0u, 0u, 0u);
					auto map = data.stageImage.memoryMap(mapBegin, lsize * layerCount);
					auto span = map.span();
					for(auto l = 0u; l < layerCount; ++l) {
						auto count = data.source->read(map.span(), m, l);
						dlg_assert(count == lsize);
						tkn::skip(span, lsize);
					}

					if(!map.coherent()) {
						map.flush();
					}
				} else { // non-tight, unpack potentially row by row
					for(auto l = 0u; l < layerCount; ++l) {
						auto span = data.source->read(m, l);
						dlg_assertm(span.size() == lsize, "{} vs {}",
							span.size(), lsize);

						vk::ImageSubresource subres;
						subres.arrayLayer = l;
						subres.mipLevel = m;
						subres.aspectMask = vk::ImageAspectBits::color;
						vpp::fillMap(data.stageImage, data.srcFormat,
							{mwidth, mheight, mdepth}, span, subres);
					}
				}
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
			blit.srcOffsets[1].z = mdepth;
			blit.dstOffsets[1].x = mwidth;
			blit.dstOffsets[1].y = mheight;
			blit.dstOffsets[1].z = mdepth;
			blits.push_back(blit);
		}

		// bring stage image into transferSrcOptimal layout for blit
		b0.image = data.stageImage;
		b0.newLayout = vk::ImageLayout::transferSrcOptimal;
		b0.dstAccessMask = vk::AccessBits::transferRead;
		b0.subresourceRange =
			{vk::ImageAspectBits::color, 0, numFillLevels, 0, layerCount};

		vk::PipelineStageBits srcStage;
		if(data.copyToStageImage) {
			b0.oldLayout = vk::ImageLayout::transferDstOptimal;
			b0.srcAccessMask = vk::AccessBits::transferWrite;
			srcStage = vk::PipelineStageBits::transfer;
		} else {
			b0.oldLayout = vk::ImageLayout::preinitialized;
			b0.srcAccessMask = vk::AccessBits::hostWrite;
			srcStage = vk::PipelineStageBits::host;
		}

		vk::cmdPipelineBarrier(cb, srcStage, vk::PipelineStageBits::transfer,
			{}, {}, {}, {{b0}});

		// nearest is enough, we are not scaling in any way
		// NOTE: we could introduce scaling (with linear or nearest filter)
		// as a feature here. Do when needed.
		vk::cmdBlitImage(cb, data.stageImage,
			vk::ImageLayout::transferSrcOptimal, data.target,
			vk::ImageLayout::transferDstOptimal, blits,
			vk::Filter::nearest);
	}

	b0.image = data.target;
	if(data.numLevels > numFillLevels) {
		DownscaleTarget target;
		target.image = data.target;
		target.format = data.srcFormat;
		target.extent = {width, height, depth};
		target.layerCount = layerCount;
		target.baseLevel = numFillLevels - 1;
		target.srcScope.access = vk::AccessBits::transferWrite;
		target.srcScope.layout = vk::ImageLayout::transferDstOptimal;
		target.srcScope.stages = vk::PipelineStageBits::transfer;

		auto count = data.numLevels - numFillLevels;
		tkn::downscale(cb, target, count);

		b0.oldLayout = vk::ImageLayout::transferSrcOptimal;
		b0.srcAccessMask = vk::AccessBits::transferWrite |
			vk::AccessBits::transferRead;
	} else {
		b0.oldLayout = vk::ImageLayout::transferDstOptimal;
		b0.srcAccessMask = vk::AccessBits::transferWrite;
	}

	b0.newLayout = data.dstScope.layout;
	b0.dstAccessMask = data.dstScope.access;
	b0.subresourceRange = {vk::ImageAspectBits::color, 0, data.numLevels, 0, layerCount};

	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
		data.dstScope.stages, {}, {}, {}, {{b0}});

	// no longer needed
	data.source = {};
}

TextureInitData createTexture(WorkBatcher& wb,
		std::unique_ptr<ImageProvider> img,
		const TextureCreateParams& params) {

	TextureInitData data = {};
	auto numLayers = img->layers();

	auto size = img->size();
	auto numLevels = img->mipLevels();
	if(params.mipLevels) {
		if(*params.mipLevels == 0) {
			numLevels = vpp::mipmapLevels({size.x, size.y, size.z});
		} else {
			numLevels = *params.mipLevels;
		}
	}

	auto numFilledLevels = params.fillMipmaps ?
		(*params.fillMipmaps ? numLevels : 1u) :
		std::min(img->mipLevels(), numLevels);

	// we only evaluate this to check if a blit might be needed
	auto dataFormat = img->format();
	if(params.srgb && isSRGB(dataFormat) != *params.srgb) {
		dlg_debug("toggling srgb");
		dataFormat = toggleSRGB(dataFormat);
	}

	auto dstFormat = params.format ? *params.format : dataFormat;
	vk::FormatFeatureFlags features {}; // TODO: we probably need more
	if(dataFormat != dstFormat || numFilledLevels > img->mipLevels()) {
		features |= vk::FormatFeatureBits::blitDst;
	}

	auto usage = params.usage | vk::ImageUsageBits::transferDst;
	if(numFilledLevels > img->mipLevels()) {
		usage |= vk::ImageUsageBits::transferSrc;
	}

	vk::ImageCreateInfo ici;
	ici.arrayLayers = numLayers;
	ici.extent = {size.x, size.y, size.z};
	ici.mipLevels = numLevels;
	ici.format = dstFormat;
	ici.usage = usage;
	ici.initialLayout = vk::ImageLayout::undefined;
	ici.imageType = minImageType(ici.extent, params.minTypeDim);
	ici.tiling = vk::ImageTiling::optimal;
	ici.samples = vk::SampleCountBits::e1;

	data.cubemap = params.cubemap ? *params.cubemap : img->cubemap();
	if(data.cubemap) {
		dlg_assertm(numLayers % 6 == 0, "A cubemap image must have 6 faces");
		dlg_assertm(params.view.layerCount % 6u == 0u, "A cubemap image view "
			"must have 6 layers (or a multiple of 6 for cubemap arrays)");

		ici.flags = vk::ImageCreateBits::cubeCompatible;
	}

	// TODO: evaluate this with all needed features.
	// When blitDst isn't supported we could fallback to cpu conversion!
	// Just need a way to signal to createFill that blitting is not allowed
	if(!vpp::supported(wb.dev, ici, features)) {
		throw std::runtime_error("Image parameters not supported by device");
	}

	auto devBits = wb.dev.deviceMemoryTypes();
	data.numLevels = numLevels;
	data.image = {data.initImage, wb.alloc.memDevice, ici, devBits};
	data.view = params.view;
	data.view.layerCount = data.view.layerCount ? data.view.layerCount : numLayers;
	data.view.levelCount = data.view.levelCount ? data.view.levelCount : numLevels;
	data.fill = createFill(wb, data.image, dstFormat, std::move(img),
		numFilledLevels, params.srgb);

	data.viewType = minImageViewType({size.x, size.y, size.z},
		data.view.layerCount, data.cubemap, params.minTypeDim);

	return data;
}

vpp::Image initImage(TextureInitData& data, WorkBatcher& wb) {
	data.image.init(data.initImage);
	doFill(data.fill, wb.cb);
	return std::move(data.image);
}

vpp::ViewableImage initTexture(TextureInitData& data, WorkBatcher& wb) {
	vk::ImageViewCreateInfo ivi;
	ivi.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	ivi.subresourceRange.layerCount = data.view.layerCount;
	ivi.subresourceRange.levelCount = data.view.levelCount;
	ivi.subresourceRange.baseArrayLayer = data.view.baseArrayLayer;
	ivi.subresourceRange.baseMipLevel = data.view.baseMipLevel;
	ivi.format = data.fill.dstFormat;
	ivi.viewType = data.viewType;

	auto img = initImage(data, wb);
	ivi.image = img;

	auto& dev = img.device();
	return {std::move(img), {dev, ivi}};
}

vpp::ViewableImage buildTexture(WorkBatcher& wb,
		std::unique_ptr<ImageProvider> img, const TextureCreateParams& params) {
	auto& dev = wb.dev;
	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family(),
		vk::CommandPoolCreateBits::transient);
	vk::beginCommandBuffer(cb, {});

	auto ocb = wb.cb;
	nytl::ScopeGuard cbGuard = [&]{ wb.cb = ocb; };
	wb.cb = cb;

	auto data = createTexture(wb, std::move(img), params);
	auto ret = initTexture(data, wb);

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));

	return ret;
}

vpp::ViewableImage buildTexture(const vpp::Device& dev,
		std::unique_ptr<ImageProvider> img, const TextureCreateParams& params) {
	auto wb = WorkBatcher(dev);
	return buildTexture(wb, std::move(img), params);
}

} // namespace tkn
