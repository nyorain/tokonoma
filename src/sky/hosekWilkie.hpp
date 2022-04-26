#pragma once

#include <tkn/types.hpp>
#include <tkn/ccam.hpp>
#include <tkn/defer.hpp>
#include <tkn/texture.hpp>
#include <tkn/f16.hpp>
#include <tkn/shader.hpp>
#include <tkn/image.hpp>
#include <tkn/render.hpp>
#include <tkn/bits.hpp>
#include <tkn/sky.hpp>
#include <tkn/scene/environment.hpp>

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

// TODO: might be useful to just use a single image with multiple
// layers instead of multiple images for the lookup tables.

class HosekWilkieSky {
public:
	// TODO: we are probably able to get away with r8Unorm format
	// here, check it out.
	static constexpr auto tableFormat = vk::Format::r16g16b16a16Sfloat;
	struct UboData {
		Mat4f transform;
		Vec3f toSun;
		float _1;
		Vec3f sunColor;
		float exposure;
		Vec3f config7;
		float cosSunSize;
		Vec3f config2;
		float roughness;
		Vec3f rad;
	};

public:
	vk::Semaphore waitSemaphore;

public:
	HosekWilkieSky() = default;
	bool init(vpp::Device& dev, vk::Sampler sampler, vk::RenderPass rp) {
		using namespace tkn::hosekSky;

		rp_ = rp;
		auto bindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, &sampler),
		};

		dsLayout_.init(dev, bindings);
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
		ubo_ = {dev.bufferAllocator(), sizeof(UboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		genSem_ = vpp::Semaphore{dev};
		genCb_ = dev.commandAllocator().get(qfam,
			vk::CommandPoolCreateBits::resetCommandBuffer);

		vk::ImageCreateInfo ici;
		ici.arrayLayers = 1u;
		ici.mipLevels = 1u;
		ici.extent = {Table::numEntries, Table::numLevels, 1U};
		ici.imageType = vk::ImageType::e2d;
		ici.initialLayout = vk::ImageLayout::undefined;
		ici.samples = vk::SampleCountBits::e1;
		ici.tiling = vk::ImageTiling::optimal;
		ici.format = tableFormat;
		ici.usage = vk::ImageUsageBits::transferDst |
			vk::ImageUsageBits::sampled;

		vk::ImageViewCreateInfo ivi;
		ivi.format = tableFormat;
		ivi.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		ivi.viewType = vk::ImageViewType::e2d;

		tables_.f = {dev.devMemAllocator(), ici, ivi};
		tables_.g = {dev.devMemAllocator(), ici, ivi};
		tables_.fhh = {dev.devMemAllocator(), ici, ivi};

		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform(ubo_);
		dsu.imageSampler(tables_.f.imageView(), vk::ImageLayout::shaderReadOnlyOptimal);
		dsu.imageSampler(tables_.g.imageView(), vk::ImageLayout::shaderReadOnlyOptimal);
		dsu.imageSampler(tables_.fhh.imageView(), vk::ImageLayout::shaderReadOnlyOptimal);
		dsu.apply();

		if(!loadPipe()) {
			return false;
		}

		return true;
	}

	void render(vk::CommandBuffer cb) {
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_.vkHandle()});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdDraw(cb, 14, 1, 0, 0); // skybox strip
	}

	bool loadPipe() {
		auto& dev = vkDevice();
		vpp::ShaderModule vert(dev, tkn_skybox_vert_data);
		auto mod = tkn::loadShader(dev, "sky/sky.frag");
		if(!mod) {
			dlg_error("Failed to reload sky.frag");
			return false;
		}

		vpp::GraphicsPipelineInfo gpi{rp_, pipeLayout_, {{{
			{vert, vk::ShaderStageBits::vertex},
			{*mod, vk::ShaderStageBits::fragment},
		}}}};

		gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
		gpi.blend.pAttachments = &tkn::noBlendAttachment();

		pipe_ = {dev, gpi.info()};
		return true;
	}


	bool updateDevice(const tkn::ControlledCamera& cam) {
		auto res = false;
		if(reloadPipe_) {
			reloadPipe_ = false;
			loadPipe();
			res = true;
		}

		if(rebuild_) {
			rebuild_ = false;

			vk::beginCommandBuffer(genCb_, {});
			buildTables(genCb_);
			vk::endCommandBuffer(genCb_);

			auto& qs = vkDevice().queueSubmitter();
			vk::SubmitInfo si;
			si.commandBufferCount = 1u;
			si.pCommandBuffers = &genCb_.vkHandle();
			si.pSignalSemaphores = &genSem_.vkHandle();
			si.signalSemaphoreCount = 1u;
			qs.add(si);

			waitSemaphore = genSem_;
		}

		auto coeff = [&](unsigned i) {
			return Vec3f{
				sky_.config.coeffs[0][i],
				sky_.config.coeffs[1][i],
				sky_.config.coeffs[2][i],
			};
		};

		UboData data;
		data.transform = cam.fixedViewProjectionMatrix();
		// data.sunColor = sunRadianceRGB(dot(sky_.toSun, Vec3f{0.f, 1.f, 0.f}), sky_.turbidity);
		// data.sunColor = 100'000.f * Vec3f{0.9f, 0.8f, 0.7f};
		data.sunColor = sunRadiance_;
		data.toSun = sky_.toSun;
		data.exposure = 0.00003f;
		data.config7 = coeff(7);
		data.cosSunSize = std::cos(0.03);
		data.config2 = coeff(2);
		data.rad = sky_.config.radiance;
		data.roughness = roughness_;

		auto map = ubo_.memoryMap();
		auto span = map.span();
		tkn::write(span, data);
		map.flush();

		return res;
	}

	void buildTables(vk::CommandBuffer cb) {
		auto& dev = vkDevice();

		using namespace tkn::hosekSky;
		constexpr auto fmt = tableFormat;

		sky_.groundAlbedo = {0.7f, 0.7f, 0.9f};
		sky_.turbidity = turbidity_;
		sky_.toSun = nytl::normalized(toSun_);
		sky_.config = bakeConfiguration(sky_.turbidity, sky_.groundAlbedo, sky_.toSun);
		auto table = generateTable(sky_);

		sunRadiance_ = (1 / 0.000068f) * tkn::Sky::sunIrradiance(sky_.turbidity, sky_.groundAlbedo, sky_.toSun);

		auto size = Vec3ui{Table::numEntries, Table::numLevels, 1u};

		using Texel = nytl::Vec<4, tkn::f16>;
		Texel dataF[Table::numLevels][Table::numEntries];
		Texel dataG[Table::numLevels][Table::numEntries];
		Texel dataFHH[Table::numLevels][Table::numEntries];
		for(auto i = 0u; i < Table::numLevels; ++i) {
			for(auto j = 0u; j < Table::numEntries; ++j) {
				auto FH = table.fh[i][j];
				auto F = table.f[i][j];
				auto G = table.g[i][j];

				dataF[i][j] = {F.x, F.y, F.z, 0.f};
				dataG[i][j] = {G.x, G.y, G.z, 0.f};
				dataFHH[i][j] = {FH.x, FH.y, FH.z, table.h[i][j]};
			}
		}

		auto imgF = tkn::wrapImage(size, fmt, tkn::bytes(dataF));
		auto imgG = tkn::wrapImage(size, fmt, tkn::bytes(dataG));
		auto imgFHH = tkn::wrapImage(size, fmt, tkn::bytes(dataFHH));

		auto wb = tkn::WorkBatcher(dev);
		wb.cb = cb;

		std::array<tkn::FillData, 3> fillDatas;
		std::array<tkn::TextureInitData, 3> initDatas;
		assert(tables_.f.image());

		fillDatas[0] = createFill(wb, tables_.f.image(), fmt, std::move(imgF), 1u);
		fillDatas[1] = createFill(wb, tables_.g.image(), fmt, std::move(imgG), 1u);
		fillDatas[2] = createFill(wb, tables_.fhh.image(), fmt, std::move(imgFHH), 1u);

		doFill(fillDatas[0], cb);
		doFill(fillDatas[1], cb);
		doFill(fillDatas[2], cb);
	}

	void toSun(const nytl::Vec3f& ndir) {
		toSun_ = ndir;
		rebuild_ = true;
	}

	const vpp::Device& vkDevice() { return pipeLayout_.device(); }

	bool key(swa_key keycode) {
		switch(keycode) {
			case swa_key_r:
				reloadPipe_ = true;
				return true;
			case swa_key_left: {
				roughness_ = std::clamp(roughness_ - 0.02, 0.0, 1.0);
				dlg_info("roughness: {}", roughness_);
				return true;
			} case swa_key_right: {
				roughness_ = std::clamp(roughness_ + 0.02, 0.0, 1.0);
				dlg_info("roughness: {}", roughness_);
				return true;
			} case swa_key_pageup: {
				turbidity_ = std::clamp(turbidity_ + 0.25, 1.0, 10.0);
				dlg_info("turbidity: {}", turbidity_);
				rebuild_ = true;
				return true;
			} case swa_key_pagedown: {
				turbidity_ = std::clamp(turbidity_ - 0.25, 1.0, 10.0);
				dlg_info("turbidity: {}", turbidity_);
				rebuild_ = true;
				return true;
			}
			default:
				return false;
		}
	}

private:
	float roughness_ {0.f};
	float turbidity_ {2.f};

	vk::RenderPass rp_;

	bool reloadPipe_ {};
	bool rebuild_ {true};
	nytl::Vec3f toSun_ {};

	vpp::CommandBuffer genCb_;
	vpp::Semaphore genSem_;

	tkn::hosekSky::Sky sky_;
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;
	nytl::Vec3f sunRadiance_;

	struct {
		tkn::hosekSky::Table table;
		vpp::ViewableImage f;
		vpp::ViewableImage g;
		vpp::ViewableImage fhh;
	} tables_;
};

