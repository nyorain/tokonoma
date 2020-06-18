#pragma once

#include "precoscat.hpp"

#include <tkn/texture.hpp>
#include <tkn/ccam.hpp>
#include <tkn/util.hpp>
#include <tkn/bits.hpp>
#include <tkn/shader.hpp>
#include <tkn/types.hpp>
#include <tkn/render.hpp>

#include <vpp/handles.hpp>
#include <vpp/debug.hpp>
#include <vpp/init.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/queue.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/shader.hpp>
#include <vpp/submit.hpp>
#include <vpp/image.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/util/file.hpp>
#include <vpp/vk.hpp>

#include <array>
#include <shaders/tkn.skybox.vert.h>

using std::move;
using nytl::constants::pi;
using namespace tkn::types;

class BrunetonSky {
public:
	u32 maxScatOrder_ {2u};

public:
	// WorkGroup extents (per dimension).
	// Aiming for a total of ~64 invorcations per WorkGroup is usually
	// a good idea. Could do less if the done work is heave.
	static constexpr auto groupDimSize2D = 8u; // transmission, ground
	static constexpr auto groupDimSize3D = 4u; // scattering

	static constexpr auto transFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto transMuSize = 512u;
	static constexpr auto transHeightSize = 128u;
	static constexpr auto transExtent = vk::Extent3D{
		transMuSize,
		transHeightSize,
		1u
	};

	// must be the same for single mie, rayleigh and multi scattering
	static constexpr auto scatFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto scatNuSize = 8u;
	static constexpr auto scatMuSSize = 32u;
	static constexpr auto scatMuSize = 128u;
	static constexpr auto scatHeightSize = 64u;
	static constexpr auto scatExtent = vk::Extent3D{
		scatNuSize * scatMuSSize,
		scatMuSize,
		scatHeightSize
	};

	static constexpr auto groundFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto groundMuSSize = 512u;
	static constexpr auto groundHeightSize = 128u;
	static constexpr auto groundExtent = vk::Extent3D{
		groundMuSSize,
		groundHeightSize,
		1u
	};

	struct UboData {
		mat4 vp; // only needed for rendering
		Atmosphere atmosphere;
		vec3 sunDir;
		float _; // padding
		vec3 viewPos;
	};

public:
	Atmosphere atmosphere;

public:
	BrunetonSky() = default;
	bool init(vpp::Device& dev, vk::Sampler sampler, vk::RenderPass rp) {
		rp_ = rp;

		// earth-like
		atmosphere.bottom = 6360000.0;
		atmosphere.top = 6420000.0;
		// atmosphere.top = 7000000.0;
		atmosphere.sunAngularRadius = 0.00935 / 2.0;
		atmosphere.minMuS = -0.2;
		atmosphere.mieG = 0.8f;
		atmosphere.scatNuSize = scatNuSize;

		const auto rayleighH = 8000.f;
		atmosphere.rayleighDensity.constantTerm = 0.f;
		atmosphere.rayleighDensity.expScale = -1.f / rayleighH;
		atmosphere.rayleighDensity.expTerm = 1.f;
		atmosphere.rayleighDensity.lienarTerm = 0.f;

		// TODO: re-add mie
		const auto mieH = 1200.f;
		atmosphere.mieDensity.constantTerm = 0.f;
		atmosphere.mieDensity.expScale = -1.f / mieH;
		atmosphere.mieDensity.expTerm = 1.f;
		atmosphere.mieDensity.lienarTerm = 0.f;

		// TODO: add ozone layer
		atmosphere.absorptionDensity = {};
		atmosphere.absorptionExtinction = {};

		// TODO: use spectral colors, build them from scratch
		atmosphere.solarIrradiance = {1.f, 1.f, 1.f};
		atmosphere.solarIrradiance *= 5.f;
		// atmosphere.mieScattering = {2e-5, 3e-5, 4e-5};
		atmosphere.mieScattering = {5e-5f, 5e-5, 5e-5};
		// atmosphere.mieScattering = {0.f, 0.f, 0.f};
		atmosphere.mieExtinction = 0.9f * atmosphere.mieScattering;
		// rayleigh scattering coefficients roughly for RGB wavelengths, from
		// http://renderwonk.com/publications/gdm-2002/GDM_August_2002.pdf
		atmosphere.rayleighScattering = {6.95e-6, 1.18e-5, 2.44e-5};

		// TODO eh not sure at all about this
		atmosphere.groundAlbedo = {0.1f, 0.1f, 0.1f, 1.f};

		auto initUbo = vpp::Init<vpp::SubBuffer>(dev.bufferAllocator(),
			sizeof(UboData), vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes());
		ubo_ = initUbo.init();

		// trans
		vk::ImageCreateInfo ici;
		ici.extent = transExtent;
		ici.arrayLayers = 1u;
		ici.format = transFormat;
		ici.mipLevels = 1u;
		ici.imageType = vk::ImageType::e2d;
		ici.initialLayout = vk::ImageLayout::undefined;
		ici.samples = vk::SampleCountBits::e1;
		ici.tiling = vk::ImageTiling::optimal;
		ici.usage = vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::storage;

		auto& alloc = dev.devMemAllocator();
		auto devTypes = dev.deviceMemoryTypes();
		auto initTrans = vpp::Init<vpp::ViewableImage>(alloc, ici, devTypes);
		auto initGround = vpp::Init<vpp::ViewableImage>(alloc, ici, devTypes);

		// scat
		ici.extent = scatExtent;
		ici.format = scatFormat;
		ici.imageType = vk::ImageType::e3d;

		auto initScatR = vpp::Init<vpp::ViewableImage>(alloc, ici, devTypes);
		auto initScatM = vpp::Init<vpp::ViewableImage>(alloc, ici, devTypes);
		auto initScatMulti = vpp::Init<vpp::ViewableImage>(alloc, ici, devTypes);
		auto initScatMultiBack = vpp::Init<vpp::ViewableImage>(alloc, ici, devTypes);
		auto initInScat = vpp::Init<vpp::ViewableImage>(alloc, ici, devTypes);
		auto initScatCombined = vpp::Init<vpp::ViewableImage>(alloc, ici, devTypes);

		// initialize
		vk::ImageViewCreateInfo ivi;
		ivi.format = transFormat;
		ivi.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		ivi.viewType = vk::ImageViewType::e2d;
		transTable_ = initTrans.init(ivi);
		groundTable_ = initGround.init(ivi);

		ivi.format = scatFormat;
		ivi.viewType = vk::ImageViewType::e3d;
		scatTableRayleigh_ = initScatR.init(ivi);
		scatTableMie_ = initScatM.init(ivi);
		scatTableMulti_ = initScatMulti.init(ivi);
		inScatTable_ = initInScat.init(ivi);
		scatTableCombined_ = initScatCombined.init(ivi);

		vpp::nameHandle(transTable_, "BrunetonSky:transTable");
		vpp::nameHandle(groundTable_, "BrunetonSky:groundTable");
		vpp::nameHandle(scatTableRayleigh_, "BrunetonSky:scatTableRayleigh");
		vpp::nameHandle(scatTableMie_, "BrunetonSky:scatTableMie");
		vpp::nameHandle(scatTableMulti_, "BrunetonSky:scatTableMulti");
		vpp::nameHandle(inScatTable_, "BrunetonSky:inScatTable");
		vpp::nameHandle(scatTableCombined_, "BrunetonSky:scatTableCombined");

		// init pipelines and stuff
		initPreTrans();
		initPreSingleScat(sampler);
		initPreGroundIrradiance(sampler);
		initPreInScat(sampler);
		initPreMultiScat(sampler);
		initRender(sampler);

		if(!loadGenPipes() || !loadRenderPipe()) {
			return false;
		}

		return true;
	}

	void initPreTrans() {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// out: computed transmittance
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		preTrans_.dsLayout.init(dev, bindings);
		vpp::nameHandle(preTrans_.dsLayout, "BrunetonSky:preTrans:dsLayout");
		preTrans_.pipeLayout = {dev, {{preTrans_.dsLayout.vkHandle()}}, {}};
		vpp::nameHandle(preTrans_.pipeLayout, "BrunetonSky:preTrans:pipeLayout");
		preTrans_.ds = {dev.descriptorAllocator(), preTrans_.dsLayout};
		vpp::nameHandle(preTrans_.ds, "BrunetonSky:preTrans:ds");

		vpp::DescriptorSetUpdate dsu(preTrans_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{}, transTable_.imageView(), vk::ImageLayout::general}});
	}

	void initPreSingleScat(vk::Sampler sampler) {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// in: transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// out: single rayleigh scattering
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
			// out: single mie scattering
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
			// out: combined scattering
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		preSingleScat_.dsLayout.init(dev, bindings);
		vpp::nameHandle(preSingleScat_.dsLayout, "BrunetonSky:preSingleScat:dsLayout");
		preSingleScat_.pipeLayout = {dev, {{preSingleScat_.dsLayout.vkHandle()}}, {}};
		vpp::nameHandle(preSingleScat_.pipeLayout, "BrunetonSky:preSingleScat:pipeLayout");
		preSingleScat_.ds = {dev.descriptorAllocator(), preSingleScat_.dsLayout};
		vpp::nameHandle(preSingleScat_.ds, "BrunetonSky:preSingleScat:ds");

		vpp::DescriptorSetUpdate dsu(preSingleScat_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.storage({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::general}});
		dsu.storage({{{}, scatTableMie_.imageView(), vk::ImageLayout::general}});
		dsu.storage({{{}, scatTableCombined_.imageView(), vk::ImageLayout::general}});
	}

	void initPreGroundIrradiance(vk::Sampler sampler) {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// in: transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// in: single rayleigh scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// in: single mie scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// in: combined multi scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// out: ground irradiance
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		preGround_.dsLayout.init(dev, bindings);
		vpp::nameHandle(preGround_.dsLayout, "BrunetonSky:preGround:dsLayout");

		vk::PushConstantRange pcr;
		pcr.size = sizeof(u32);
		pcr.stageFlags = vk::ShaderStageBits::compute;
		preGround_.pipeLayout = {dev, {{preGround_.dsLayout.vkHandle()}}, {{pcr}}};
		vpp::nameHandle(preGround_.pipeLayout, "BrunetonSky:preGround:pipeLayout");

		preGround_.ds = {dev.descriptorAllocator(), preGround_.dsLayout};
		vpp::nameHandle(preGround_.ds, "BrunetonSky:preGround:ds");

		vpp::DescriptorSetUpdate dsu(preGround_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableMie_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableMulti_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.storage({{{}, groundTable_.imageView(), vk::ImageLayout::general}});
	}

	void initPreInScat(vk::Sampler sampler) {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// in: transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// in: single rayleigh scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// in: single mie scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// in: combined multi scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// in: ground irradiance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// out: in scattering
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};


		preInScat_.dsLayout.init(dev, bindings);
		vpp::nameHandle(preInScat_.dsLayout, "BrunetonSky:preInScat:dsLayout");

		vk::PushConstantRange pcr;
		pcr.size = sizeof(u32);
		pcr.stageFlags = vk::ShaderStageBits::compute;
		preInScat_.pipeLayout = {dev, {{preInScat_.dsLayout.vkHandle()}}, {{pcr}}};
		vpp::nameHandle(preInScat_.pipeLayout, "BrunetonSky:preInScat:pipeLayout");

		preInScat_.ds = {dev.descriptorAllocator(), preInScat_.dsLayout};
		vpp::nameHandle(preInScat_.ds, "BrunetonSky:preInScat:ds");

		vpp::DescriptorSetUpdate dsu(preInScat_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableMie_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableMulti_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, groundTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.storage({{{}, inScatTable_.imageView(), vk::ImageLayout::general}});
	}

	void initPreMultiScat(vk::Sampler sampler) {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// in: transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// in: inScat (multi scattering step 1)
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			// out: multi scattering
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
			// out: combined scattering
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		preMultiScat_.dsLayout.init(dev, bindings);
		vpp::nameHandle(preMultiScat_.dsLayout, "BrunetonSky:preMultiScat:dsLayout");
		preMultiScat_.pipeLayout = {dev, {{preMultiScat_.dsLayout.vkHandle()}}, {}};
		vpp::nameHandle(preMultiScat_.pipeLayout, "BrunetonSky:preMultiScat:pipeLayout");
		preMultiScat_.ds = {dev.descriptorAllocator(), preMultiScat_.dsLayout};
		vpp::nameHandle(preMultiScat_.ds, "BrunetonSky:preMultiScat:ds");

		vpp::DescriptorSetUpdate dsu(preMultiScat_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, inScatTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.storage({{{}, scatTableMulti_.imageView(), vk::ImageLayout::general}});
		dsu.storage({{{}, scatTableCombined_.imageView(), vk::ImageLayout::general}});
	}

	void initRender(vk::Sampler sampler) {
		auto& dev = device();
		auto bindings = std::array{
			// atmosphere parameters
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// tranmission
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
			// scat rayleigh
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
			// scat mie
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
			// combined scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
			// ground texture
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
		};

		render_.dsLayout.init(dev, bindings);
		vpp::nameHandle(render_.dsLayout, "BrunetonSky:render:dsLayout");

		vk::PushConstantRange pcr;
		pcr.size = sizeof(u32);
		pcr.stageFlags = vk::ShaderStageBits::fragment;
		render_.pipeLayout = {dev, {{render_.dsLayout.vkHandle()}}, {{pcr}}};
		vpp::nameHandle(render_.pipeLayout, "BrunetonSky:render:pipeLayout");

		render_.ds = {dev.descriptorAllocator(), render_.dsLayout};
		vpp::nameHandle(render_.ds, "BrunetonSky:render:ds");

		vpp::DescriptorSetUpdate dsu(render_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableMie_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableCombined_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, groundTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});

		// debugging, for seeing muli scat textures in renderdoc.
		// The first three textures are not used for multi-scat-lookup anyways.
		// dsu.uniform({{{ubo_}}});
		// dsu.imageSampler({{{}, inScatTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		// dsu.imageSampler({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		// dsu.imageSampler({{{}, scatTableMie_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		// dsu.imageSampler({{{}, scatTableMulti_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		// dsu.imageSampler({{{}, groundTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
	}

	bool loadGenPipes() {
		auto& dev = device();
		auto preTrans = tkn::loadShader(dev, "sky/preTrans.comp");
		auto preSingleScat = tkn::loadShader(dev, "sky/preSingleScat.comp");
		auto preMultiScat = tkn::loadShader(dev, "sky/preMultiScat.comp");
		auto preInScat = tkn::loadShader(dev, "sky/preInScat.comp");
		auto preGround = tkn::loadShader(dev, "sky/preGround.comp");
		if(!preTrans || !preSingleScat || !preMultiScat) {
			dlg_error("Failed to reload/compile pcs computation shaders");
			return false;
		}


		vk::ComputePipelineCreateInfo cpi;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;

		// 2D
		tkn::ComputeGroupSizeSpec specTrans(groupDimSize2D, groupDimSize2D);
		cpi.layout = preTrans_.pipeLayout;
		cpi.stage.module = *preTrans;
		cpi.stage.pSpecializationInfo = &specTrans.spec;
		preTrans_.pipe = {dev, cpi};

		cpi.layout = preGround_.pipeLayout;
		cpi.stage.module = *preGround;
		preGround_.pipe = {dev, cpi};

		// 3D
		auto sgds = groupDimSize3D;
		tkn::ComputeGroupSizeSpec specScat(sgds, sgds, sgds);
		cpi.layout = preSingleScat_.pipeLayout;
		cpi.stage.module = *preSingleScat;
		cpi.stage.pSpecializationInfo = &specScat.spec;
		preSingleScat_.pipe = {dev, cpi};

		cpi.layout = preMultiScat_.pipeLayout;
		cpi.stage.module = *preMultiScat;
		preMultiScat_.pipe = {dev, cpi};

		cpi.layout = preInScat_.pipeLayout;
		cpi.stage.module = *preInScat;
		preInScat_.pipe = {dev, cpi};

		return true;
	}

	bool loadRenderPipe() {
		auto& dev = device();
		vpp::ShaderModule vert(dev, tkn_skybox_vert_data);
		auto mod = tkn::loadShader(dev, "sky/sky-pcs.frag");
		if(!mod) {
			dlg_error("Failed to reload/compile sky-pcs.frag");
			return false;
		}

		vpp::GraphicsPipelineInfo gpi{rp_, render_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{*mod, vk::ShaderStageBits::fragment},
		}}}};

		gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
		gpi.blend.pAttachments = &tkn::noBlendAttachment();

		render_.pipe = {dev, gpi.info()};
		return true;
	}

	void recordGroundGen(vk::CommandBuffer cb, u32 scatOrder) {
		auto sgx = tkn::ceilDivide(scatExtent.width, groupDimSize3D);
		auto sgy = tkn::ceilDivide(scatExtent.height, groupDimSize3D);
		auto sgz = tkn::ceilDivide(scatExtent.depth, groupDimSize3D);

		// discard old content if there is any
		vk::ImageMemoryBarrier bGround;
		bGround.image = groundTable_.image();
		bGround.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		bGround.oldLayout = vk::ImageLayout::undefined;
		bGround.srcAccessMask = vk::AccessBits::shaderRead;
		bGround.newLayout = vk::ImageLayout::general;
		bGround.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader, {}, {}, {}, {{bGround}});

		// compute
		vk::cmdPushConstants(cb, preGround_.pipeLayout,
			vk::ShaderStageBits::compute, 0u, sizeof(u32), &scatOrder);
		tkn::cmdBindComputeDescriptors(cb, preGround_.pipeLayout, 0u, {preGround_.ds});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, preGround_.pipe);
		vk::cmdDispatch(cb, sgx, sgy, sgz);

		bGround.srcAccessMask = vk::AccessBits::shaderWrite;
		bGround.oldLayout = vk::ImageLayout::general;
		bGround.dstAccessMask = vk::AccessBits::shaderRead;
		bGround.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader | vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{bGround}});
	}

	void recordMultiScatGen(vk::CommandBuffer cb, u32 scatOrder) {
		auto sgx = tkn::ceilDivide(scatExtent.width, groupDimSize3D);
		auto sgy = tkn::ceilDivide(scatExtent.height, groupDimSize3D);
		auto sgz = tkn::ceilDivide(scatExtent.depth, groupDimSize3D);

		// 1: inScat
		vk::ImageMemoryBarrier bInScat;
		bInScat.image = inScatTable_.image();
		bInScat.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		bInScat.oldLayout = vk::ImageLayout::undefined;
		bInScat.srcAccessMask = {};
		bInScat.newLayout = vk::ImageLayout::general;
		bInScat.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe | vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader, {}, {}, {}, {{bInScat}});

		vk::cmdPushConstants(cb, preInScat_.pipeLayout,
			vk::ShaderStageBits::compute, 0u, sizeof(u32), &scatOrder);
		tkn::cmdBindComputeDescriptors(cb, preInScat_.pipeLayout, 0u, {preInScat_.ds});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, preInScat_.pipe);
		vk::cmdDispatch(cb, sgx, sgy, sgz);

		bInScat.srcAccessMask = vk::AccessBits::shaderWrite;
		bInScat.oldLayout = vk::ImageLayout::general;
		bInScat.dstAccessMask = vk::AccessBits::shaderRead;
		bInScat.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{bInScat}});

		// 2: multiScat
		vk::ImageMemoryBarrier bMulti;
		bMulti.image = scatTableMulti_.image();
		bMulti.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		bMulti.oldLayout = vk::ImageLayout::undefined;
		bMulti.srcAccessMask = vk::AccessBits::shaderRead;
		bMulti.newLayout = vk::ImageLayout::general;
		bMulti.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::ImageMemoryBarrier bC;
		bC.image = scatTableCombined_.image();
		bC.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		bC.oldLayout = vk::ImageLayout::general;
		bC.newLayout = vk::ImageLayout::general;
		bC.srcAccessMask = vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite;
		bC.dstAccessMask = vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{bMulti, bC}});

		tkn::cmdBindComputeDescriptors(cb, preMultiScat_.pipeLayout, 0u, {preMultiScat_.ds});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, preMultiScat_.pipe);
		vk::cmdDispatch(cb, sgx, sgy, sgz);

		bMulti.srcAccessMask = vk::AccessBits::shaderWrite;
		bMulti.oldLayout = vk::ImageLayout::general;
		bMulti.dstAccessMask = vk::AccessBits::shaderRead;
		bMulti.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader | vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{bMulti}});
	}

	void generate(vk::CommandBuffer cb) {
		vk::ImageMemoryBarrier bT, bM, bR, bC;
		bT.image = transTable_.image();
		bT.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		bT.oldLayout = vk::ImageLayout::undefined;
		bT.srcAccessMask = {};
		bT.newLayout = vk::ImageLayout::general;
		bT.dstAccessMask = vk::AccessBits::shaderWrite;

		bM = bR = bC = bT;
		bM.image = scatTableMie_.image();
		bR.image = scatTableRayleigh_.image();
		bC.image = scatTableCombined_.image();

		// bring multi scat table into readOnly layout
		// this is needed because the first iteration expects it in this format
		// even though it doesn't read it.
		auto bMulti = bT;
		bMulti.image = scatTableMulti_.image();
		bMulti.dstAccessMask = vk::AccessBits::shaderRead;
		bMulti.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;

		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::computeShader,
			{}, {}, {}, {{bT, bM, bR, bC, bMulti}});

		// #1: calculate transmission
		auto tgx = tkn::ceilDivide(transExtent.width, groupDimSize2D);
		auto tgy = tkn::ceilDivide(transExtent.width, groupDimSize2D);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, preTrans_.pipe);
		tkn::cmdBindComputeDescriptors(cb, preTrans_.pipeLayout, 0, {preTrans_.ds});
		vk::cmdDispatch(cb, tgx, tgy, 1u);

		// Make sure writing the scatter lut is visible. We need its
		// contents to compute the scattering tables.
		// Also bring it into shaderReadOnly format. Also use the fragment
		// shader as dst stage for rendering later on.
		bT.srcAccessMask = bT.dstAccessMask;
		bT.oldLayout = bT.newLayout;
		bT.dstAccessMask = vk::AccessBits::shaderRead;
		bT.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader |
			vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{bT}});

		// #2: calculate scattering
		auto sgx = tkn::ceilDivide(scatExtent.width, groupDimSize3D);
		auto sgy = tkn::ceilDivide(scatExtent.height, groupDimSize3D);
		auto sgz = tkn::ceilDivide(scatExtent.depth, groupDimSize3D);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, preSingleScat_.pipe);
		tkn::cmdBindComputeDescriptors(cb, preSingleScat_.pipeLayout,
			0, {preSingleScat_.ds});
		vk::cmdDispatch(cb, sgx, sgy, sgz);

		// Make computation of the scattering tables visible and bring
		// into shaderReadOnly layout as well for rendering.
		bM = bR = bT;
		bM.image = scatTableMie_.image();
		bR.image = scatTableRayleigh_.image();
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::topOfPipe | vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::computeShader | vk::PipelineStageBits::fragmentShader,
			{}, {}, {}, {{bM, bR}});

		recordGroundGen(cb, 1u);
		for(auto i = 2u; i <= maxScatOrder_; ++i) {
			recordMultiScatGen(cb, i);
			recordGroundGen(cb, i);
		}

		// final transition for combined scattering
		bC.oldLayout = vk::ImageLayout::general;
		bC.newLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		bC.srcAccessMask = vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite;
		bC.dstAccessMask = vk::AccessBits::shaderRead;

		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{bC}});
	}

	void render(vk::CommandBuffer cb) {
		vk::cmdPushConstants(cb, render_.pipeLayout,
			vk::ShaderStageBits::fragment, 0u, sizeof(u32), &maxScatOrder_);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, render_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, render_.pipeLayout, 0, {render_.ds});
		vk::cmdDraw(cb, 14u, 1, 0, 0); // skybox triangle strip
	}

	bool updateDevice(const tkn::ControlledCamera& cam,
			bool reloadRender, bool reloadGen, nytl::Vec3f sunDir) {
		auto res = false;
		if(reloadRender) {
			loadRenderPipe();
			res = true;
		}

		if(reloadGen) {
			loadGenPipes();
			res = true;
		}

		auto map = ubo_.memoryMap();
		auto span = map.span();

		UboData data;
		data.vp = cam.fixedViewProjectionMatrix();
		data.atmosphere = atmosphere;
		data.sunDir = sunDir;
		data.viewPos = cam.position();
		tkn::write(span, data);

		map.flush();

		return res;
	}

	Vec3f startViewPos() const {
		return (atmosphere.bottom + 5000.f) * Vec3f{0.f, 1.f, 0.f}; // north pole
	}

	const vpp::Device& device() const { return ubo_.device(); }

private:
	vk::RenderPass rp_;
	vpp::SubBuffer ubo_;

	vpp::ViewableImage transTable_;
	vpp::ViewableImage scatTableRayleigh_;
	vpp::ViewableImage scatTableMie_;

	// needed for multiple scattering
	vpp::ViewableImage scatTableMulti_;
	vpp::ViewableImage inScatTable_; // first step of multi-scattering precomp

	// what we get in the end
	vpp::ViewableImage groundTable_; // ground irradiance
	vpp::ViewableImage scatTableCombined_;

	// NOTE: we can probably combine some of the dsLayout's and pipeLayout's
	// later on. Especially for just precomputation.
	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} render_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} preTrans_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} preSingleScat_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} preInScat_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} preMultiScat_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} preGround_;
};
