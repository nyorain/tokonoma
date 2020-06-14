#pragma once

#include "precoscat.hpp"

#include <tkn/texture.hpp>
#include <tkn/ccam.hpp>
#include <tkn/bits.hpp>
#include <tkn/shader.hpp>
#include <tkn/types.hpp>
#include <tkn/render.hpp>

#include <vpp/handles.hpp>
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
	static constexpr auto transGroupDimSize = 8u; // 2D
	static constexpr auto transFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto transMuSize = 512u;
	static constexpr auto transHeightSize = 128u;
	static constexpr auto transExtent = vk::Extent3D{
		transMuSize,
		transHeightSize,
		1u
	};

	static constexpr auto scatGroupDimSize = 4u; // 3D
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
		atmosphere.mieScattering = {2e-5, 3e-5, 4e-5};
		// atmosphere.mieScattering = {5e-5f, 5e-5, 5e-5};
		// atmosphere.mieScattering = {0.f, 0.f, 0.f};
		atmosphere.mieExtinction = 0.9f * atmosphere.mieScattering;
		// rayleigh scattering coefficients roughly for RGB wavelengths, from
		// http://renderwonk.com/publications/gdm-2002/GDM_August_2002.pdf
		atmosphere.rayleighScattering = {6.95e-6, 1.18e-5, 2.44e-5};

		auto uboSize = sizeof(Atmosphere) + sizeof(Mat4f) +
			2 * sizeof(Vec3f) + sizeof(u32);
		ubo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

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

		vk::ImageViewCreateInfo ivi;
		ivi.format = transFormat;
		ivi.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		ivi.viewType = vk::ImageViewType::e2d;

		transTable_ = {dev.devMemAllocator(), ici, ivi};

		// scat
		ici.extent = scatExtent;
		ici.format = scatFormat;
		ici.imageType = vk::ImageType::e3d;

		ivi.format = scatFormat;
		ivi.viewType = vk::ImageViewType::e3d;

		scatTableRayleigh_ = {dev.devMemAllocator(), ici, ivi};
		scatTableMie_ = {dev.devMemAllocator(), ici, ivi};

		// init pipelines and stuff
		initPreTrans();
		initPreScat(sampler);
		initRender(sampler);

		if(!loadGenPipes() || !loadRenderPipe()) {
			return false;
		}

		return true;
	}

	void initPreTrans() {
		auto& dev = device();
		auto bindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		preTrans_.dsLayout.init(dev, bindings);
		preTrans_.pipeLayout = {dev, {{preTrans_.dsLayout.vkHandle()}}, {}};
		preTrans_.ds = {dev.descriptorAllocator(), preTrans_.dsLayout};

		vpp::DescriptorSetUpdate dsu(preTrans_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{}, transTable_.imageView(), vk::ImageLayout::general}});
	}

	void initPreScat(vk::Sampler sampler) {
		auto& dev = device();
		auto bindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::compute, &sampler),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		preScat_.dsLayout.init(dev, bindings);
		preScat_.pipeLayout = {dev, {{preScat_.dsLayout.vkHandle()}}, {}};
		preScat_.ds = {dev.descriptorAllocator(), preScat_.dsLayout};

		vpp::DescriptorSetUpdate dsu(preScat_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.storage({{{}, scatTableRayleigh_.imageView(), vk::ImageLayout::general}});
		dsu.storage({{{}, scatTableMie_.imageView(), vk::ImageLayout::general}});
	}

	void initRender(vk::Sampler sampler) {
		auto& dev = device();
		auto bindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
		};

		render_.dsLayout.init(dev, bindings);
		render_.pipeLayout = {dev, {{render_.dsLayout.vkHandle()}}, {}};
		render_.ds = {dev.descriptorAllocator(), render_.dsLayout};

		vpp::DescriptorSetUpdate dsu(render_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.imageSampler({{{}, transTable_.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableRayleigh_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, scatTableMie_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});
	}

	bool loadGenPipes() {
		auto& dev = device();
		auto preTrans = tkn::loadShader(dev, "sky/preTrans.comp");
		auto preScat = tkn::loadShader(dev, "sky/preScat.comp");
		if(!preTrans || !preScat) {
			dlg_error("Failed to reload/compile pcs computation shaders");
			return false;
		}

		tkn::ComputeGroupSizeSpec specTrans(transGroupDimSize, transGroupDimSize);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = preTrans_.pipeLayout;
		cpi.stage.module = *preTrans;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &specTrans.spec;
		cpi.stage.stage = vk::ShaderStageBits::compute;
		preTrans_.pipe = {dev, cpi};

		auto sgds = scatGroupDimSize;
		tkn::ComputeGroupSizeSpec specScat(sgds,sgds,sgds);
		cpi.layout = preScat_.pipeLayout;
		cpi.stage.module = *preScat;
		cpi.stage.pSpecializationInfo = &specScat.spec;
		preScat_.pipe = {dev, cpi};

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

	void generate(vk::CommandBuffer cb) {
		vk::ImageMemoryBarrier bT, bM, bR;
		bT.image = transTable_.image();
		bT.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		bT.oldLayout = vk::ImageLayout::undefined;
		bT.srcAccessMask = {};
		bT.newLayout = vk::ImageLayout::general;
		bT.dstAccessMask = vk::AccessBits::shaderWrite;

		bM = bR = bT;
		bM.image = scatTableMie_.image();
		bR.image = scatTableRayleigh_.image();

		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::computeShader, {}, {}, {}, {{bT, bM, bR}});

		// #1: calculate transmission
		auto tgx = std::ceil(transExtent.width / float(transGroupDimSize));
		auto tgy = std::ceil(transExtent.height / float(transGroupDimSize));

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
		auto sgx = std::ceil(scatExtent.width / float(scatGroupDimSize));
		auto sgy = std::ceil(scatExtent.height / float(scatGroupDimSize));
		auto sgz = std::ceil(scatExtent.depth / float(scatGroupDimSize));

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, preScat_.pipe);
		tkn::cmdBindComputeDescriptors(cb, preScat_.pipeLayout, 0, {preScat_.ds});
		vk::cmdDispatch(cb, sgx, sgy, sgz);

		// Make computation of the scattering tables visible and bring
		// into shaderReadOnly layout as well for rendering.
		bM = bR = bT;
		bM.image = scatTableMie_.image();
		bR.image = scatTableRayleigh_.image();
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::fragmentShader, {}, {}, {}, {{bM, bR}});
	}

	void render(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, render_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, render_.pipeLayout, 0, {render_.ds});
		vk::cmdDraw(cb, 16, 1, 0, 0); // skybox triangle strip
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
		tkn::write(span, cam.fixedViewProjectionMatrix());
		tkn::write(span, atmosphere);
		tkn::write(span, sunDir);
		tkn::write(span, u32(scatNuSize));
		tkn::write(span, cam.position());
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
	} preScat_;

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} preTrans_;
};
