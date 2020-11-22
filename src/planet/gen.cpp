#include <tkn/headless.hpp>
#include <tkn/f16.hpp>
#include <tkn/image.hpp>
#include <tkn/util.hpp>
#include <tkn/bits.hpp>
#include <tkn/render.hpp>
#include <tkn/types.hpp>
#include <tkn/transform.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/formats.hpp>
#include <vpp/image.hpp>
#include <vpp/queue.hpp>
#include <vpp/submit.hpp>
#include <tkn/shader.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <iostream>
#include <array>
#include <shaders/planet.gen.comp.h>

using namespace tkn::types;

struct PCRData {
	nytl::Vec3f x;
	u32 face;
	nytl::Vec3f y;
	float _pad {};
	nytl::Vec3f z;
};

// NOTE: r16f isn't guaranteed to be supported for storage images.
// We use rgba16f atm anyways to store normals.
int main(int argc, const char** argv) {
	dlg_trace("Setup");

	// parse arguments
	auto parser = tkn::HeadlessArgs::defaultParser();
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

	// init headless setup
	auto args = tkn::HeadlessArgs(result);
	auto headless = tkn::Headless(args);

	auto& dev = *headless.device;
	auto bindings = std::array {
		vpp::descriptorBinding(vk::DescriptorType::storageImage),
	};

	auto dsLayout = vpp::TrDsLayout(dev, bindings);

	vk::PushConstantRange pcr;
	pcr.offset = 0u;
	pcr.size = sizeof(PCRData);
	pcr.stageFlags = vk::ShaderStageBits::compute;
	auto pipeLayout = vpp::PipelineLayout(dev, {{dsLayout}}, {{pcr}});
	auto ds = vpp::TrDs(dev.descriptorAllocator(), dsLayout);

	auto groupDimSize = 8u;
	vpp::ShaderModule mod(dev, planet_gen_comp_data);
	tkn::ComputeGroupSizeSpec specTrans(groupDimSize, groupDimSize);
	vk::ComputePipelineCreateInfo cpi;
	cpi.stage.pName = "main";
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.layout = pipeLayout;
	cpi.stage.module = mod;
	cpi.stage.pSpecializationInfo = &specTrans.spec;
	auto pipe = vpp::Pipeline(dev, cpi);

	auto format = vk::Format::r16g16b16a16Sfloat;
	auto size = 2 * 2048u;
	auto extent = vk::Extent2D{size, size};
	auto vi = vpp::ViewableImageCreateInfo(format,
		vk::ImageAspectBits::color, extent,
		vk::ImageUsageBits::storage | vk::ImageUsageBits::transferSrc);
	vi.img.flags = vk::ImageCreateBits::cubeCompatible;
	vi.img.arrayLayers = 6u;
	vi.view.viewType = vk::ImageViewType::cube;
	vi.view.subresourceRange.layerCount = 6u;
	auto cubemap = vpp::ViewableImage(dev.devMemAllocator(), vi,
		dev.deviceMemoryTypes());

	auto dsu = vpp::DescriptorSetUpdate(ds);
	dsu.storage({{{}, cubemap.imageView(), vk::ImageLayout::general}});
	dsu.apply();

	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());

	vk::beginCommandBuffer(cb, {});

	vk::ImageMemoryBarrier barrier;
	barrier.image = cubemap.image();
	barrier.oldLayout = vk::ImageLayout::undefined;
	barrier.srcAccessMask = {};
	barrier.newLayout = vk::ImageLayout::general;
	barrier.dstAccessMask = vk::AccessBits::shaderWrite;
	barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 6};
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
		vk::PipelineStageBits::computeShader, {}, {}, {}, {{barrier}});

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe);
	tkn::cmdBindComputeDescriptors(cb, pipeLayout, 0u, {ds});

	auto dc = tkn::ceilDivide(size, groupDimSize);
	for(auto i = 0u; i < 6u; ++i) {
		PCRData pcd;
		pcd.face = i;
		pcd.x = tkn::cubemap::faces[i].s;
		pcd.y = tkn::cubemap::faces[i].t;
		pcd.z = tkn::cubemap::faces[i].dir;
		vk::cmdPushConstants(cb, pipeLayout, vk::ShaderStageBits::compute, 0u,
			sizeof(pcd), &pcd);
		vk::cmdDispatch(cb, dc, dc, 1u);
	}

	barrier.oldLayout = vk::ImageLayout::general;
	barrier.srcAccessMask = vk::AccessBits::shaderWrite;
	barrier.newLayout = vk::ImageLayout::transferSrcOptimal;
	barrier.dstAccessMask = vk::AccessBits::transferRead;
	vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
		vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});
	auto stage = vpp::retrieveStagingLayers(cb, cubemap.image(),
		format, vk::ImageLayout::transferSrcOptimal,
		{size, size, 1u}, {vk::ImageAspectBits::color, 0, 0, 6});

	vk::endCommandBuffer(cb);

	dlg_trace("Executing commands");
	qs.wait(qs.add(cb));

	// save
	dlg_trace("Saving image");
	auto map = stage.memoryMap();
	auto provider = tkn::wrapImage({size, size, 1u}, format, 1, 6u,
		map.span(), true);

	auto res = tkn::writeKtx("heightmap.ktx", *provider);
	dlg_assertm(res == tkn::WriteError::none, (int) res);
}
