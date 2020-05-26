#include <tkn/headless.hpp>
#include <tkn/bits.hpp>
#include <tkn/types.hpp>
#include <tkn/render.hpp>
#include <tkn/features.hpp>
#include <tkn/image.hpp>
#include <tkn/texture.hpp>
#include <tkn/scene/pbr.hpp>
#include <tkn/scene/environment.hpp>

#include <vpp/vk.hpp>
#include <vpp/formats.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/debug.hpp>
#include <vpp/submit.hpp>
#include <vpp/init.hpp>
#include <vpp/queue.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/image.hpp>
#include <vpp/util/file.hpp>

#include <dlg/dlg.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>

#include <shaders/tkn.brdflut.comp.h>
#include <iostream>
#include <fstream>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tkn/stb_image_write.h>

using namespace tkn::types;

vpp::Sampler linearSampler(const vpp::Device& dev);
void saveBrdf(const char* outfile, const vpp::Device& dev);
void saveCubemap(const char* equirect, const char* outfile,
	const vpp::Device& dev);
void saveIrradiance(const char* cubemap, const char* outfile,
	const vpp::Device& dev);
void saveConvoluted(const char* cubemap, const char* outfile,
	const vpp::Device& dev);
void saveSHProj(const char* cubemap, const char* outfile,
	const vpp::Device& dev);
int saveSkies(float turbidity, const char* outEnv, const char* outData,
	const vpp::Device& dev);

int main(int argc, const char** argv) {
	auto parser = tkn::HeadlessArgs::defaultParser();
	parser.definitions.push_back({
		"output", {"--output", "-o"},
		"The file to write the output to. Otherwise default is used", 1});

	// commands
	parser.definitions.push_back({
		"brdflut", {"--brdflut"},
		"Write a brdf specular IBL lookup table to brdflut.ktx", 0});
	parser.definitions.push_back({
		"cubemap", {"--cubemap"},
		"Generate a cubemap from an equirect environment map", 1});
	parser.definitions.push_back({
		"irradiance", {"--irradiance"},
		"Load the given environment map and generate an irradiance map", 1});
	parser.definitions.push_back({
		"convolute", {"--convolute"},
		"Convolute a cube environment map for specular IBL", 1});
	parser.definitions.push_back({
		"shproj", {"--shproj"},
		"Project an irradiance map onto spherical harmonics", 1});
	parser.definitions.push_back({
		"bakesky", {"--bakesky"},
		"Bakes environment maps and irradiance spherical harmonics data for "
			"hosek-wilkie skies for given turbidity", 1});

	auto usage = std::string("Usage: ") + argv[0] + " [options]\n\n";
	argagg::parser_results result;
	try {
		result = parser.parse(argc, argv);
	} catch(const std::exception& error) {
		argagg::fmt_ostream help(std::cerr);
		help << usage << parser << "\n";
		help << "Invalid arguments: " << error.what();
		help << std::endl;
		return 1;
	}

	if(result["help"]) {
		argagg::fmt_ostream help(std::cerr);
		help << usage << parser << std::endl;
		return 0;
	}

	auto args = tkn::HeadlessArgs(result);
	if(result.has_option("bakesky")) {
		args.featureChecker = [](tkn::Features& enable, const tkn::Features& supported) {
			if(!supported.base.features.imageCubeArray) {
				dlg_fatal("Required feature 'imageCubeArray' not supported");
				return false;
			}

			enable.base.features.imageCubeArray = true;
			return true;
		};
	}

	auto headless = tkn::Headless(args);

	auto& dev = *headless.device;
	std::optional<const char*> output;
	if(result.has_option("output")) {
		output = result["output"];
	}

	if(result.has_option("brdflut")) {
		auto out = output.value_or("brdflut.ktx");
		saveBrdf(out, dev);
	} else if(result.has_option("cubemap")) {
		auto out = output.value_or("cubemap.ktx");
		saveCubemap(result["cubemap"], out, dev);
	} else if(result.has_option("convolute")) {
		auto out = output.value_or("convolution.ktx");
		saveConvoluted(result["convolute"], out, dev);
	} else if(result.has_option("irradiance")) {
		auto out = output.value_or("irradiance.ktx");
		saveIrradiance(result["irradiance"], out, dev);
	} else if(result.has_option("shproj")) {
		auto out = output.value_or("sh.bin");
		saveSHProj(result["shproj"], out, dev);
	} else if(result.has_option("bakesky")) {
		float turbidity = result["bakesky"].as<float>();
		auto outEnv = output.value_or("skyEnvs.ktx");
		auto outData = "skyData.bin"; // TODO
		return saveSkies(turbidity, outEnv, outData, dev);
	} else {
		dlg_fatal("No/unsupported command given!");
		argagg::fmt_ostream help(std::cerr);
		help << usage << parser << std::endl;
		return -1;
	}

	return 0;
}


// BRDF lut
// NOTE: we don't need an hdr format since the values are always
// between 0.0 and 1.0.
// TODO: in some parts, the map is detailed, in others not at all.
// we can probably get away with reducing the texture size to
// something like 128x128 (or even smaller) but that may bring
// issues in the higher detail areas... we could run a function
// over the inputs first? like a pow or something? we need high precision
// for low values so if we map with pow(input, 2) (or even more) we should
// be ok with a smaller texture size.
// we linearly interpolate when using the texture.
// try it out though
//
// A small image size is important since the texture may be almost
// randomly sampled for surfaces with a highly varying normal
// map i guess... (or highly varying roughness but that's not so
// common)
void saveBrdf(const char* filename, const vpp::Device& dev) {
	constexpr static auto size = 512u;

	// gen brdf
	auto memBits = dev.deviceMemoryTypes();
	auto format = vk::Format::r8g8b8a8Unorm;
	auto usage = vk::ImageUsageBits::storage |
		vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::transferSrc;
	auto info = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, {size, size}, usage);
	auto sampleCount = 1024u;

	dlg_assert(vpp::supported(dev, info.img));
	auto lut = vpp::ViewableImage(dev.devMemAllocator(), info, memBits);
	vpp::nameHandle(lut.image(), "BRDF Lookup Table");

	// init pipeline
	auto bindings = {
		vpp::descriptorBinding( // output image, irradiance
			vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
	};

	auto dsLayout = vpp::TrDsLayout{dev, bindings};
	auto pipeLayout = vpp::PipelineLayout{dev, {{dsLayout.vkHandle()}}, {}};

	constexpr auto groupDimSize = 8u;
	// constexpr auto sampleCount = 1024u;
	// spec constants for work group size seem to have problems in
	// validation layers...
	std::array<vk::SpecializationMapEntry, 3> entries = {{
		{0, 0, 4u},
		{1, 4u, 4u},
		{2, 8u, 4u},
	}};

	std::byte constData[12u];
	auto span = nytl::Span<std::byte>(constData);
	tkn::write(span, u32(groupDimSize));
	tkn::write(span, u32(groupDimSize));
	tkn::write(span, u32(sampleCount));

	vk::SpecializationInfo spec;
	spec.dataSize = sizeof(constData);
	spec.pData = constData;
	spec.mapEntryCount = entries.size();
	spec.pMapEntries = entries.data();

	vpp::ShaderModule shader(dev, tkn_brdflut_comp_data);
	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout;
	cpi.stage.pSpecializationInfo = &spec;
	cpi.stage.module = shader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "main";

	vk::Pipeline vkpipe;
	vk::createComputePipelines(dev, {}, 1u, cpi, nullptr, vkpipe);
	auto pipe = vpp::Pipeline(dev, vkpipe);

	// ds
	auto ds = vpp::TrDs(dev.descriptorAllocator(), dsLayout);
	vpp::DescriptorSetUpdate dsu(ds);
	dsu.storage({{{}, lut.vkImageView(), vk::ImageLayout::general}});
	dsu.apply();

	// record
	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());

	vk::beginCommandBuffer(cb, {});

	vk::ImageMemoryBarrier barrier;
	barrier.image = lut.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout, 0, {{ds.vkHandle()}}, {});
	auto count = std::ceil(size / float(groupDimSize));
	vk::cmdDispatch(cb, count, count, 1);

	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

	auto stage = vpp::retrieveStaging(cb, lut.image(), format,
		vk::ImageLayout::transferSrcOptimal, {size, size, 1u},
		{vk::ImageAspectBits::color, 0, 0});

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));

	// write image
	auto map = stage.memoryMap();
	if(!map.coherent()) {
		map.invalidate();
	}

	// auto ptr = reinterpret_cast<float*>(map.ptr());
	// stbi_write_png(filename, size, size, 4, map.ptr(), size * 4);
	auto provider = tkn::wrap({size, size}, format, map.span());
	auto res = tkn::writeKtx(filename, *provider);
	dlg_assertm(res == tkn::WriteError::none, (int) res);
}

// TODO: might move that to tkn
vpp::Sampler linearSampler(const vpp::Device& dev) {
	vk::SamplerCreateInfo sci;
	sci.addressModeU = vk::SamplerAddressMode::clampToEdge;
	sci.addressModeV = vk::SamplerAddressMode::clampToEdge;
	sci.addressModeW = vk::SamplerAddressMode::clampToEdge;
	sci.magFilter = vk::Filter::linear;
	sci.minFilter = vk::Filter::linear;
	sci.mipmapMode = vk::SamplerMipmapMode::linear;
	sci.minLod = 0.0;
	sci.maxLod = 100.0;
	sci.anisotropyEnable = false;
	return {dev, sci};
}

void saveCubemap(const char* equirectPath, const char* outfile,
		const vpp::Device& dev) {
	auto format = vk::Format::r16g16b16a16Sfloat;
	auto size = 1024u;
	tkn::TextureCreateParams params;
	params.format = format;
	params.mipLevels = 1u;
	auto equirect = tkn::Texture(dev, tkn::read(equirectPath, true), params);

	auto sampler = linearSampler(dev);
	tkn::Cubemapper cubemapper;
	cubemapper.init(dev.devMemAllocator(), {size, size}, sampler);

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());

	vk::beginCommandBuffer(cb, {});
	cubemapper.record(cb, equirect.imageView());
	auto cubemap = cubemapper.finish();

	vk::ImageMemoryBarrier barrier;
	barrier.image = cubemap.image();
	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferRead;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 6};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

	auto stage = vpp::retrieveStagingLayers(cb, cubemap.image(),
		format, vk::ImageLayout::transferSrcOptimal,
		{size, size, 1u}, {vk::ImageAspectBits::color, 0, 0, 6});

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));

	// write image
	auto map = stage.memoryMap();
	if(!map.coherent()) {
		map.invalidate();
	}

	auto faceSize = vpp::formatSize(format) * size * size;
	dlg_assert(map.size() == faceSize * 6);
	const auto* const ptr = map.ptr();
	auto ptrs = {
		ptr + 0 * faceSize,
		ptr + 1 * faceSize,
		ptr + 2 * faceSize,
		ptr + 3 * faceSize,
		ptr + 4 * faceSize,
		ptr + 5 * faceSize,
	};

	auto provider = tkn::wrap({size, size}, format, 1, 1, 6, {ptrs});
	auto res = tkn::writeKtx(outfile, *provider);
	dlg_assertm(res == tkn::WriteError::none, (int) res);
}

void saveIrradiance(const char* infile, const char* outfile,
		const vpp::Device& dev) {
	auto format = vk::Format::r16g16b16a16Sfloat;
	auto size = 32u;
	tkn::TextureCreateParams params;
	params.format = format;
	params.cubemap = true;
	auto envmap = tkn::Texture(dev, tkn::read(infile, true), params);

	auto sampler = linearSampler(dev);
	tkn::Irradiancer irradiancer;
	irradiancer.init(dev.devMemAllocator(), {size, size}, sampler);

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());

	vk::beginCommandBuffer(cb, {});
	irradiancer.record(cb, envmap.imageView());
	auto irradiance = irradiancer.finish();

	vk::ImageMemoryBarrier barrier;
	barrier.image = irradiance.image();
	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferRead;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 6};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

	auto stage = vpp::retrieveStagingLayers(cb, irradiance.image(),
		format, vk::ImageLayout::transferSrcOptimal,
		{size, size, 1u}, {vk::ImageAspectBits::color, 0, 0, 6u});

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));

	// write image
	auto map = stage.memoryMap();
	if(!map.coherent()) {
		map.invalidate();
	}

	auto faceSize = vpp::formatSize(format) * size * size;
	dlg_assert(map.size() == faceSize * 6);
	const auto* const ptr = map.ptr();
	auto ptrs = {
		ptr + 0 * faceSize,
		ptr + 1 * faceSize,
		ptr + 2 * faceSize,
		ptr + 3 * faceSize,
		ptr + 4 * faceSize,
		ptr + 5 * faceSize,
	};

	auto provider = tkn::wrap({size, size}, format, 1, 1, 6, {ptrs});
	auto res = tkn::writeKtx(outfile, *provider);
	dlg_assertm(res == tkn::WriteError::none, (int) res);
}

void saveConvoluted(const char* cubemapFile, const char* outfile,
		const vpp::Device& dev) {
	auto format = vk::Format::r16g16b16a16Sfloat;
	auto p = tkn::read(cubemapFile, true);
	auto size = p->size();
	auto full = int(vpp::mipmapLevels({size.x, size.y})) - 4;
	unsigned mipLevels = std::max(full, 1);
	dlg_assert(mipLevels > 1);

	tkn::TextureCreateParams params;
	params.format = format;
	params.cubemap = true;
	params.mipLevels = 0; // full chain
	params.fillMipmaps = true; // fill full chain
	params.usage = params.defaultUsage;
	auto cubemap = tkn::Texture(dev, std::move(p), params);

	auto vi = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, {size.x, size.y},
		vk::ImageUsageBits::storage | vk::ImageUsageBits::transferSrc);
	vi.img.mipLevels = mipLevels;
	vi.img.flags = vk::ImageCreateBits::cubeCompatible;
	vi.img.arrayLayers = 6u;
	vpp::Image envMap = {dev.devMemAllocator(), vi.img,
		dev.deviceMemoryTypes()};

	auto sampler = linearSampler(dev);
	tkn::EnvironmentMapFilter convoluter;
	convoluter.create(dev, sampler, 4096);

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());

	vk::beginCommandBuffer(cb, {});

	vk::ImageMemoryBarrier barrier;
	barrier.image = envMap;
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange =
		{vk::ImageAspectBits::color, 0, mipLevels, 0, 6};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

	auto mips = convoluter.record(cb, cubemap.imageView(), envMap,
		mipLevels, size);

	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferRead;
	barrier.subresourceRange =
		{vk::ImageAspectBits::color, 0, mipLevels, 0, 6};

	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

	auto fmtSize = vpp::formatSize(format);
	// over approximation of all mip levels (times 2)
	auto totalSize = 2 * 6u * size.x * size.y * fmtSize;
	auto align = dev.properties().limits.optimalBufferCopyOffsetAlignment;
	align = std::max<vk::DeviceSize>(align, fmtSize);
	auto stage = vpp::SubBuffer(dev.bufferAllocator(), totalSize,
		vk::BufferUsageBits::transferDst, dev.hostMemoryTypes(), align);
	auto map = stage.memoryMap();
	auto off = 0u;

	std::vector<const std::byte*> pointers;
	std::vector<vk::BufferImageCopy> copies;
	pointers.reserve(6 * mipLevels);
	copies.reserve(mipLevels);

	for(auto m = 0u; m < mipLevels; ++m) {
		nytl::Vec2ui msize;
		msize.x = std::max(size.x >> m, 1u);
		msize.y = std::max(size.y >> m, 1u);
		auto faceSize = fmtSize * msize.x * msize.y;

		auto& copy = copies.emplace_back();
		copy.bufferOffset = stage.offset() + off;
		copy.imageExtent = {msize.x, msize.y, 1u};
		copy.imageSubresource.aspectMask = vk::ImageAspectBits::color;
		copy.imageSubresource.baseArrayLayer = 0u;
		copy.imageSubresource.layerCount = 6u;
		copy.imageSubresource.mipLevel = m;

		for(auto f = 0u; f < 6u; ++f) {
			pointers.push_back(map.ptr() + off + f * faceSize);
		}

		off += faceSize * 6;
		dlg_assert(off <= totalSize);
	}

	vk::cmdCopyImageToBuffer(cb, envMap,
		vk::ImageLayout::transferSrcOptimal, stage.buffer(), copies);

	vk::BufferMemoryBarrier bbarrier;
	bbarrier.buffer = stage.buffer();
	bbarrier.offset = stage.offset();
	bbarrier.size = stage.size();
	bbarrier.srcAccessMask = vk::AccessBits::transferWrite;
	bbarrier.dstAccessMask = vk::AccessBits::hostRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
		vk::PipelineStageBits::host, {}, {}, {{bbarrier}}, {});

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));

	if(!map.coherent()) {
		map.invalidate();
	}

	auto provider = tkn::wrap(size, format, mipLevels, 1, 6, pointers);
	auto res = tkn::writeKtx(outfile, *provider);
	dlg_assertm(res == tkn::WriteError::none, (int) res);
}

void saveSHProj(const char* cubemap, const char* outfile,
		const vpp::Device& dev) {
	auto format = vk::Format::r16g16b16a16Sfloat;
	auto p = tkn::read(cubemap, true);
	auto size = p->size();
	// we mip down to the level before 32x32
	auto maxLevels = int(vpp::mipmapLevels({size.x, size.y})) - 5;
	unsigned mipLevels = std::max(maxLevels, 1);
	auto lastSize = tkn::mipmapSize({size.x, size.y}, mipLevels - 1);

	tkn::TextureCreateParams params;
	params.format = format;
	params.cubemap = true;
	params.mipLevels = mipLevels;
	params.fillMipmaps = true;
	params.usage =
		params.defaultUsage | // not really needed though
		vk::ImageUsageBits::transferSrc |
		vk::ImageUsageBits::transferDst;
	auto envmap = tkn::Texture(dev, std::move(p), params);

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});

	// copy the downscaled version to an image with exact size 32x32
	// the sh compute shader can't handle anything else at the moment
	auto info = vpp::ViewableImageCreateInfo(vk::Format::r16g16b16a16Sfloat,
		vk::ImageAspectBits::color, {32, 32},
		vk::ImageUsageBits::transferDst | vk::ImageUsageBits::sampled);
	info.img.flags = vk::ImageCreateBits::cubeCompatible;
	info.img.arrayLayers = 6;
	info.view.viewType = vk::ImageViewType::cube;
	dlg_assert(vpp::supported(dev, info.img));
	vpp::ViewableImage img(dev.devMemAllocator(), info);

	// barriers
	vk::ImageMemoryBarrier barriers[2];
	barriers[0].image = envmap.image();
	barriers[0].subresourceRange = {vk::ImageAspectBits::color, mipLevels - 1, 1, 0, 6};
	barriers[0].oldLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	barriers[0].newLayout = vk::ImageLayout::transferSrcOptimal;
	barriers[0].dstAccessMask = vk::AccessBits::transferRead;

	barriers[1].image = img.image();
	barriers[1].subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 6};
	barriers[1].oldLayout = vk::ImageLayout::undefined;
	barriers[1].newLayout = vk::ImageLayout::transferDstOptimal;
	barriers[1].dstAccessMask = vk::AccessBits::transferWrite;

	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::transfer, {}, {}, {}, barriers);

	vk::ImageBlit blit;
	blit.srcOffsets[1] = {i32(lastSize.width), i32(lastSize.height), 1};
	blit.srcSubresource = {vk::ImageAspectBits::color, mipLevels - 1, 0, 6};
	blit.dstOffsets[1] = {32, 32, 1};
	blit.dstSubresource = {vk::ImageAspectBits::color, 0, 0, 6};
	vk::cmdBlitImage(cb, envmap.image(), vk::ImageLayout::transferSrcOptimal,
		img.vkImage(), vk::ImageLayout::transferDstOptimal, {{blit}},
		vk::Filter::linear);

	barriers[1].oldLayout = vk::ImageLayout::transferDstOptimal;
	barriers[1].newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
	barriers[1].srcAccessMask = vk::AccessBits::transferWrite;
	barriers[1].dstAccessMask = vk::AccessBits::shaderRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barriers[1]}});

	auto sampler = linearSampler(dev);
	tkn::SHProjector shproj;
	shproj.create(dev, sampler);

	shproj.record(cb, img.imageView());

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));

	// read coeffs
	auto map = shproj.coeffsBuffer().memoryMap();
	auto span = map.span();
	auto coeffs = tkn::read<std::array<nytl::Vec3f, 9>>(span);
	for(auto i = 0u; i < coeffs.size(); ++i) {
		dlg_info("coeffs[{}]: {}", i, coeffs[i]);
	}

	auto file = std::ofstream(outfile,
		std::ios_base::out | std::ios_base::binary);
	if(!file.is_open()) {
		dlg_fatal("Failed to open {}", outfile);
		return;
	}

	file.write(reinterpret_cast<const char*>(&coeffs), sizeof(coeffs));
}

int saveSkies(float turbidity, const char* outSkies, const char* outData,
		const vpp::Device& dev) {
	using nytl::constants::pi;
	if(turbidity < 1.f || turbidity > 10.f) {
		dlg_fatal("Invalid turbitity: {}", turbidity);
		return -1;
	}

	constexpr auto format = vk::Format::r16g16b16a16Sfloat;
	constexpr auto mipLevels = 3u;
	struct SkyData {
		tkn::SH9<Vec4f> skyRadiance; // cosine lobe not applied yet
		Vec3f sunIrradiance;
		Vec3f sunDir;
	};

	auto sampler = linearSampler(dev);
	tkn::EnvironmentMapFilter filter;
	filter.create(dev, sampler, 128);

	// TODO: these should be parameters.
	// Maybe load via configuration file?
	// also make configurable:
	// - Sky::faceWidth
	// - Sky::faceHeight
	// - Sky::sunSize
	constexpr auto steps = 50u;
	constexpr auto faceWidth = tkn::Sky::faceWidth;
	constexpr auto faceHeight = tkn::Sky::faceHeight;
	constexpr auto groundAlbedo = Vec3f{0.4, 0.8, 1.0};
	auto sunDir = [](float t) {
		// standing at the equator, north pole is pointing straight up
		return normalized(Vec3f{0.f, std::cos(t), std::sin(t)});

		// don't really get why this doesn't work
		// float theta = t;
		// float phi = 0.45 * pi * std::sin(t);
		// return Vec3f{
		// 	std::cos(theta) * std::cos(phi),
		// 	std::sin(phi),
		// 	std::sin(theta) * std::cos(phi),
		// };
	};

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());
	vk::beginCommandBuffer(cb, {});

	// auto wb = tkn::WorkBatcher::createDefault(dev);
	// wb.cb = cb;

	vk::ImageCreateInfo ic;
	ic.format = format;
	ic.extent = {faceWidth, faceHeight, 1u};
	ic.imageType = vk::ImageType::e2d;
	ic.usage = vk::ImageUsageBits::storage | vk::ImageUsageBits::transferSrc;
	ic.samples = vk::SampleCountBits::e1;
	ic.initialLayout = vk::ImageLayout::undefined;
	ic.arrayLayers = 6u * steps;
	ic.flags = vk::ImageCreateBits::cubeCompatible;
	ic.mipLevels = mipLevels;
	auto initCombined = vpp::Init<vpp::Image>(dev.devMemAllocator(), ic,
		dev.deviceMemoryTypes());

	std::vector<tkn::Sky> skies;
	std::vector<SkyData> datas;
	skies.reserve(steps);
	datas.reserve(steps);
	for(auto i = 0u; i < steps; ++i) {
		auto t = float(2 * pi * float(i) / steps);
		auto dir = sunDir(t);

		// TODO: defer uploading and stuff as well
		auto& sky = skies.emplace_back(dev, nullptr, dir, groundAlbedo, turbidity);

		auto& data = datas.emplace_back();
		data.sunDir = -dir;
		data.sunIrradiance = sky.sunIrradiance();
		data.skyRadiance = sky.skyRadiance();
	}

	vpp::writeFile(outData, tkn::bytes(datas), true);

	auto combined = initCombined.init();
	std::vector<std::vector<tkn::EnvironmentMapFilter::Mip>> mips;

	vk::ImageMemoryBarrier barrier;
	barrier.image = combined;
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, mipLevels, 0, 6 * steps};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

	for(auto i = 0u; i < steps; ++i) {
		auto& sky = skies[i];

		auto m = filter.record(cb, sky.cubemap().imageView(), combined,
			mipLevels, {faceWidth, faceHeight}, i * 6);
		mips.push_back(std::move(m));
	}

	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

	auto stage = vpp::retrieveStagingRange(cb, combined, format,
		vk::ImageLayout::transferSrcOptimal, {faceWidth, faceHeight, 1u},
		{vk::ImageAspectBits::color, 0, mipLevels, 0, 6 * steps});

	vk::endCommandBuffer(cb);
	qs.wait(qs.add(cb));

	auto map = stage.memoryMap();
	auto fmtSize = vpp::formatSize(format);
	std::vector<const std::byte*> ptrs;
	auto debugOff = 0u;
	for(auto mip = 0u; mip < mipLevels; ++mip) {
		auto w = std::max(faceWidth >> mip, 1u);
		auto h = std::max(faceHeight >> mip, 1u);
		auto faceSize = w * h * fmtSize;

		for(auto l = 0u; l < 6 * steps; ++l) {
			auto off = vpp::imageBufferOffset(fmtSize,
				{faceWidth, faceHeight, 1u}, 6u * steps, mip, l);
			dlg_assertm(debugOff == off, "{}, {}: {} vs {}", mip, l, debugOff, off);
			ptrs.push_back(map.ptr() + off);

			debugOff += faceSize;
		}
	}

	auto provider = tkn::wrap({faceWidth, faceHeight}, format,
		mipLevels, steps, 6u, ptrs);
	auto res = tkn::writeKtx(outSkies, *provider);
	dlg_assertm(res == tkn::WriteError::none, (int) res);

	return 0;
}
