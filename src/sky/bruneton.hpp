#pragma once

#include "precoscat.hpp"

#include <tkn/texture.hpp>
#include <tkn/color.hpp>
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

#include <qwe/parse.hpp>
#include <qwe/util.hpp>
#include <array>

#include <shaders/tkn.skybox.vert.h>

using std::move;
using nytl::constants::pi;
using namespace tkn::types;

template<typename T, std::size_t N>
struct qwe::ValueParser<nytl::Vec<N, T>> {
	static std::optional<nytl::Vec<N, T>> call(const Value& value) {
		nytl::Vec<N, T> res;
		return qwe::readRange(res.begin(), res.end(), value) ?
			std::optional(res) : std::nullopt;
	}
};

class BrunetonSky {
public:
	static constexpr auto configFile = "atmosphere.qwe";

	static constexpr auto transFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto scatFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto groundFormat = vk::Format::r16g16b16a16Sfloat;

	struct UboData {
		mat4 vp; // only needed for rendering
		Atmosphere atmosphere;
		vec3 toSun;
		float _; // padding
		vec3 viewPos;
	};

public:
	nytl::Vec3f toSun {};
	vk::Semaphore waitSemaphore {};

public:
	struct {
		u32 groupDimSize2D = 8u;
		u32 groupDimSize3D = 4u;

		u32 transMuSize = 512u;
		u32 transHeightSize = 128u;

		// the same for single mie, rayleigh and multi scattering tables
		u32 scatNuSize = 8u;
		u32 scatMuSSize = 32u;
		u32 scatMuSize = 128u;
		u32 scatHeightSize = 64u;

		u32 groundMuSSize = 512u;
		u32 groundHeightSize = 128u;
		u32 maxScatOrder = 2u;

		Atmosphere atmos {};
	} config_;

public:
	BrunetonSky() = default;
	bool init(vpp::Device& dev, vk::Sampler sampler, vk::RenderPass rp) {
		sampler_ = sampler;
		rp_ = rp;

		auto initUbo = vpp::Init<vpp::SubBuffer>(dev.bufferAllocator(),
			sizeof(UboData), vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes());
		ubo_ = initUbo.init();

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		genSem_ = vpp::Semaphore{dev};
		genCb_ = dev.commandAllocator().get(qfam,
			vk::CommandPoolCreateBits::resetCommandBuffer);

		// init layouts
		initPreTrans();
		initPreSingleScat();
		initPreGroundIrradiance();
		initPreInScat();
		initPreMultiScat();
		initRender();

		// load config
		if(!loadConfig()) {
			return false;
		}

		initTables();
		updateDescriptors();
		if(!loadGenPipes() || !loadRenderPipe()) {
			return false;
		}

		return true;
	}

	void initTables() {
		auto& dev = device();

		// trans
		vk::ImageCreateInfo ici;
		ici.extent = transExtent();
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
		ici.extent = scatExtent();
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
	}

	void initPreSingleScat() {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// in: transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
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
	}

	void initPreGroundIrradiance() {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// in: transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
			// in: single rayleigh scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
			// in: single mie scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
			// in: combined multi scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
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
	}

	void initPreInScat() {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// in: transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
			// in: single rayleigh scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
			// in: single mie scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
			// in: combined multi scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
			// in: ground irradiance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
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
	}

	void initPreMultiScat() {
		auto& dev = device();
		auto bindings = std::array{
			// in: atmosphere data
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// in: transmittance
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
			// in: inScat (multi scattering step 1)
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler_),
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
	}

	void initRender() {
		auto& dev = device();
		auto bindings = std::array{
			// atmosphere parameters
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			// tranmission
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_),
			// scat rayleigh
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_),
			// scat mie
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_),
			// combined scattering
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_),
			// ground texture
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler_),
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
	}

	void updateDescriptors() {
		vpp::DescriptorSetUpdate dsuRender(render_.ds);
		dsuRender.uniform({{{ubo_}}});
		dsuRender.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuRender.imageSampler({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuRender.imageSampler({{{}, scatTableMie_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuRender.imageSampler({{{}, scatTableCombined_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuRender.imageSampler({{{}, groundTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});

		// debugging, for seeing muli scat textures in renderdoc.
		// The first three textures are not used for multi-scat-lookup anyways.
		// dsu.uniform({{{ubo_}}});
		// dsu.imageSampler({{{}, inScatTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		// dsu.imageSampler({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		// dsu.imageSampler({{{}, scatTableMie_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		// dsu.imageSampler({{{}, scatTableMulti_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		// dsu.imageSampler({{{}, groundTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});

		vpp::DescriptorSetUpdate dsuMScat(preMultiScat_.ds);
		dsuMScat.uniform({{{ubo_}}});
		dsuMScat.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuMScat.imageSampler({{{}, inScatTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuMScat.storage({{{}, scatTableMulti_.imageView(), vk::ImageLayout::general}});
		dsuMScat.storage({{{}, scatTableCombined_.imageView(), vk::ImageLayout::general}});

		vpp::DescriptorSetUpdate dsuInScat(preInScat_.ds);
		dsuInScat.uniform({{{ubo_}}});
		dsuInScat.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuInScat.imageSampler({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuInScat.imageSampler({{{}, scatTableMie_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuInScat.imageSampler({{{}, scatTableMulti_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuInScat.imageSampler({{{}, groundTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuInScat.storage({{{}, inScatTable_.imageView(), vk::ImageLayout::general}});

		vpp::DescriptorSetUpdate dsuIrrad(preGround_.ds);
		dsuIrrad.uniform({{{ubo_}}});
		dsuIrrad.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuIrrad.imageSampler({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuIrrad.imageSampler({{{}, scatTableMie_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuIrrad.imageSampler({{{}, scatTableMulti_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuIrrad.storage({{{}, groundTable_.imageView(), vk::ImageLayout::general}});

		vpp::DescriptorSetUpdate dsuTrans(preTrans_.ds);
		dsuTrans.uniform({{{ubo_}}});
		dsuTrans.storage({{{}, transTable_.imageView(), vk::ImageLayout::general}});

		vpp::DescriptorSetUpdate dsuSScat(preSingleScat_.ds);
		dsuSScat.uniform({{{ubo_}}});
		dsuSScat.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsuSScat.storage({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::general}});
		dsuSScat.storage({{{}, scatTableMie_.imageView(), vk::ImageLayout::general}});
		dsuSScat.storage({{{}, scatTableCombined_.imageView(), vk::ImageLayout::general}});
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
		tkn::ComputeGroupSizeSpec specTrans(config_.groupDimSize2D, config_.groupDimSize2D);
		cpi.layout = preTrans_.pipeLayout;
		cpi.stage.module = *preTrans;
		cpi.stage.pSpecializationInfo = &specTrans.spec;
		preTrans_.pipe = {dev, cpi};

		cpi.layout = preGround_.pipeLayout;
		cpi.stage.module = *preGround;
		preGround_.pipe = {dev, cpi};

		// 3D
		auto sgds = config_.groupDimSize3D;
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
		auto sgx = tkn::ceilDivide(scatExtent().width, config_.groupDimSize3D);
		auto sgy = tkn::ceilDivide(scatExtent().height, config_.groupDimSize3D);
		auto sgz = tkn::ceilDivide(scatExtent().depth, config_.groupDimSize3D);

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
		auto sgx = tkn::ceilDivide(scatExtent().width, config_.groupDimSize3D);
		auto sgy = tkn::ceilDivide(scatExtent().height, config_.groupDimSize3D);
		auto sgz = tkn::ceilDivide(scatExtent().depth, config_.groupDimSize3D);

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
		auto tgx = tkn::ceilDivide(transExtent().width, config_.groupDimSize2D);
		auto tgy = tkn::ceilDivide(transExtent().width, config_.groupDimSize2D);

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
		auto sgx = tkn::ceilDivide(scatExtent().width, config_.groupDimSize3D);
		auto sgy = tkn::ceilDivide(scatExtent().height, config_.groupDimSize3D);
		auto sgz = tkn::ceilDivide(scatExtent().depth, config_.groupDimSize3D);

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
		for(auto i = 2u; i <= config_.maxScatOrder; ++i) {
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

	void recordGenCb() {
		vk::beginCommandBuffer(genCb_, {});
		generate(genCb_);
		vk::endCommandBuffer(genCb_);
	}

	void render(vk::CommandBuffer cb) {
		vk::cmdPushConstants(cb, render_.pipeLayout,
			vk::ShaderStageBits::fragment, 0u, sizeof(u32), &config_.maxScatOrder);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, render_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, render_.pipeLayout, 0, {render_.ds});
		vk::cmdDraw(cb, 14u, 1, 0, 0); // skybox triangle strip
	}

	// Returns whether a rerecord is needed
	bool updateDevice(const tkn::ControlledCamera& cam) {
		auto res = false;
		if(reloadRenderPipe_) {
			reloadRenderPipe_ = false;
			loadRenderPipe();
			res = true;
		}

		if(reloadGenPipes_) {
			reloadGenPipes_ = false;
			loadGenPipes();
			recordGenCb();
			res = true;

			regen_ = true;
		}

		if(recreateTables_) {
			recreateTables_ = false;
			initTables();
			updateDescriptors();
			recordGenCb();
			res = true;
			regen_ = true;
		}

		if(regen_) {
			regen_ = false;
			vk::SubmitInfo si;
			si.commandBufferCount = 1u;
			si.pCommandBuffers = &genCb_.vkHandle();
			si.pSignalSemaphores = &genSem_.vkHandle();
			si.signalSemaphoreCount = 1u;

			auto& qs = device().queueSubmitter();
			qs.add(si);
			waitSemaphore = genSem_;
		}

		auto map = ubo_.memoryMap();
		auto span = map.span();

		UboData data;
		data.vp = cam.fixedViewProjectionMatrix();
		data.atmosphere = config_.atmos;
		data.toSun = toSun;
		data.viewPos = cam.position();
		tkn::write(span, data);
		map.flush();

		return res;
	}

	bool loadConfig() {
		qwe::Value parsed;
		try {
			auto str = tkn::readFile(configFile);
			if(str.empty()) {
				dlg_warn("Can't read {}", configFile);
				return false;
			}

			qwe::Parser parser{str};
			auto res = qwe::parseTableOrArray(parser);
			if(auto err = std::get_if<qwe::Error>(&res)) {
				dlg_warn("Error parsing {}: {}", configFile, qwe::print(*err));
				return false;
			}

			parsed = std::move(std::get<qwe::NamedValue>(res).value);
		} catch(const std::exception& err) {
			dlg_warn("Error reading/parsing {}: {}", configFile, err.what());
			return false;
		}

		try {
			// meta
			readT(config_.maxScatOrder, parsed, "meta.maxScatterOrder");
			readT(config_.transMuSize, parsed, "meta.transmission.muSize");
			readT(config_.transHeightSize, parsed, "meta.transmission.heightSize");

			readT(config_.scatNuSize, parsed, "meta.scatter.nuSize");
			readT(config_.scatMuSSize, parsed, "meta.scatter.muSSize");
			readT(config_.scatMuSize, parsed, "meta.scatter.muSize");
			readT(config_.scatHeightSize, parsed, "meta.scatter.heightSize");

			readT(config_.groundMuSSize, parsed, "meta.ground.muSSize");
			readT(config_.groundHeightSize, parsed, "meta.ground.heightSize");

			if(qwe::asT<bool>(parsed, "meta.recreateTables")) {
				recreateTables_ = true;
			}

			if(qwe::asT<bool>(parsed, "meta.reloadGen")) {
				reloadGenPipes_ = true;
			}

			// atmosphere
			auto& atmos = config_.atmos;
			auto& pa = atT(parsed, "atmosphere");

			readT(atmos.bottom, pa, "bottom");
			readT(atmos.top, pa, "top");
			readT(atmos.sunAngularRadius, pa, "sunAngularRadius");
			readT(atmos.minMuS, pa, "minMuS");
			atmos.groundAlbedo = Vec4f(qwe::asT<nytl::Vec3f>(pa, "groundAlbedo"));

			// mie
			float mieH;
			readT(mieH, pa, "mie.scaleHeight");
			atmos.mieDensity.constantTerm = 0.f;
			atmos.mieDensity.expScale = -1.f / mieH;
			atmos.mieDensity.expTerm = 1.f;
			atmos.mieDensity.lienarTerm = 0.f;
			readT(atmos.mieG, pa, "mie.g");

			auto mieScatUse = qwe::asT<std::string_view>(pa, "mie.scattering.use");
			if(mieScatUse == "rgb") {
				atmos.mieExtinction = Vec4f(qwe::asT<nytl::Vec3f>(pa, "mie.scattering.rgb"));
				atmos.mieScattering = 0.9f * atmos.mieExtinction;
			} else {
				dlg_assert(mieScatUse == "compute");
				auto alpha = qwe::asT<double>(pa, "mie.scattering.alpha");
				auto beta = qwe::asT<double>(pa, "mie.scattering.beta");
				auto albedo = qwe::asT<double>(pa, "mie.scattering.scatterAlbedo");
				generateMieScattering(alpha, beta, mieH, albedo,
					atmos.mieScattering, atmos.mieExtinction);
			}

			// rayleigh
			float rayleighH;
			readT(rayleighH, pa, "rayleigh.scaleHeight");
			atmos.rayleighDensity.constantTerm = 0.f;
			atmos.rayleighDensity.expScale = -1.f / rayleighH;
			atmos.rayleighDensity.expTerm = 1.f;
			atmos.rayleighDensity.lienarTerm = 0.f;

			auto rayleighScatUse = qwe::asT<std::string_view>(pa, "rayleigh.scattering.use");
			if(rayleighScatUse == "rgb") {
				atmos.rayleighScattering = Vec4f(qwe::asT<nytl::Vec3f>(pa, "rayleigh.scattering.rgb"));
			} else {
				dlg_assert(rayleighScatUse == "compute");
				auto strength = qwe::asT<double>(pa, "rayleigh.scattering.strength");
				atmos.rayleighScattering = Vec4f(generateRayleigh(strength));
			}

			// ozone
			readT(atmos.absorptionPeak, pa, "ozone.peak");
			if(qwe::asT<bool>(pa, "ozone.enable")) {
				// TODO: make configurable
				atmos.absorptionDensity0 = {};
				atmos.absorptionDensity1.lienarTerm = 1.0 / 15000.0;
				atmos.absorptionDensity1.constantTerm = -2.0 / 3.0;

				atmos.absorptionDensity1 = {};
				atmos.absorptionDensity1.lienarTerm = -1.0 / 15000.0;
				atmos.absorptionDensity1.constantTerm = 8.0 / 3.0;

				constexpr auto dobsonUnit = 2.687e20;
				constexpr auto maxOzoneNumberDensity = 300.0 * dobsonUnit / 15000.0;
				auto ozone = parseSpectral(atT(pa, "ozone.scattering.crossSection"));
				for(auto& val : ozone.samples) {
					val *= maxOzoneNumberDensity;
				}

				atmos.absorptionExtinction = Vec4f(tkn::XYZtoRGB(toXYZ(ozone)));
			} else {
				atmos.absorptionExtinction = {};
			}


			// solar irradiance
			auto siUse = qwe::asT<std::string_view>(pa, "solarIrradiance.use");
			if(siUse == "rgb") {
				atmos.solarIrradiance = Vec4f(qwe::asT<nytl::Vec3f>(pa, "solarIrradiance.rgb"));
			} else {
				dlg_assert(siUse == "spectral");
				auto spectral = parseSpectral(atT(pa, "solarIrradiance.spectral"));
				atmos.solarIrradiance = Vec4f(tkn::XYZtoRGB(toXYZ(spectral)));
			}

			atmos.scatNuSize = config_.scatNuSize;
		} catch(const std::exception& err) {
			dlg_warn("Error processing {}: {}", configFile, err.what());
			return false;
		}

		return true;
	}

	static nytl::Vec3f generateRayleigh(float strength) {
		tkn::SpectralColor spectral;
		for(auto i = 0u; i < tkn::SpectralColor::nSamples; ++i) {
			auto l = tkn::SpectralColor::wavelength(i);
			spectral.samples[i] = strength * std::pow(l * 1e-3, -4.0);
		}

		return tkn::XYZtoRGB(toXYZ(spectral));
	}

	static void generateMieScattering(double alpha, double beta,
			double scaleHeight, double scatterAlbedo,
			nytl::Vec4f& outScatter, nytl::Vec4f& outAbsorption) {

		tkn::SpectralColor scatter, absorption;
		outScatter = {};
		outAbsorption = {};
		for(auto i = 0u; i < tkn::SpectralColor::nSamples; ++i) {
			auto l = tkn::SpectralColor::wavelength(i);
			double mie = beta / scaleHeight * std::pow(l * 1e-3, -alpha);
			scatter.samples[i] = scatterAlbedo * mie;
			absorption.samples[i] = mie;
		}

		outScatter = Vec4f(tkn::XYZtoRGB(toXYZ(scatter)));
		outAbsorption = Vec4f(tkn::XYZtoRGB(toXYZ(absorption)));
	}

	static tkn::SpectralColor parseSpectral(const qwe::Value& value) {
		float lStart, lEnd;
		readT(lStart, value, "start");
		readT(lEnd, value, "end");

		std::vector<float> spectrum;
		readT(spectrum, value, "values");
		return tkn::SpectralColor::fromSamples(spectrum.data(), spectrum.size(),
			lStart, lEnd);
	}

	Vec3f startViewPos() const {
		return (config_.atmos.bottom + 500.f) * Vec3f{0.f, 1.f, 0.f}; // north pole
	}

	const vpp::Device& device() const { return ubo_.device(); }

	vk::Extent3D transExtent() const {
		return {config_.transMuSize, config_.transHeightSize, 1u};
	}
	vk::Extent3D scatExtent() const {
		auto w = config_.scatNuSize * config_.scatMuSSize;
		return {w, config_.scatMuSize, config_.scatHeightSize};
	}
	vk::Extent3D groundExtent() const {
		return {config_.groundMuSSize, config_.groundHeightSize, 1u};
	}

	bool key(swa_key keycode) {
		switch(keycode) {
			case swa_key_left: {
				auto order = config_.maxScatOrder;
				order = std::clamp(order - 1u, 1u, 10u);
				dlg_info("maxScatOrder: {}", order);
				config_.maxScatOrder = order;
				regen_ = true;
				return true;
			} case swa_key_right: {
				auto order = config_.maxScatOrder;
				order = std::clamp(order + 1u, 1u, 10u);
				dlg_info("maxScatOrder: {}", order);
				config_.maxScatOrder = order;
				regen_ = true;
				return true;
			} case swa_key_r:
				reloadRenderPipe_ = true;
				return true;
			case swa_key_c:
				loadConfig();
				return true;
			case swa_key_t:
				reloadGenPipes_ = true;
				return true;
			default:
				return false;
		}
	}

private:
	vk::Sampler sampler_;
	vk::RenderPass rp_;
	vpp::SubBuffer ubo_;

	bool reloadRenderPipe_ {}; // whether to reload render pipe on updateDevice
	bool reloadGenPipes_ {}; // whether to recreate gen pipes on updateDevice
	bool regen_ {true}; // whether to regen on updateDevice
	bool recreateTables_ {}; // whether to recreate tables on updateDevice

	vpp::Semaphore genSem_;
	vpp::CommandBuffer genCb_;

	// tables
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

