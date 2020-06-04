#include <tkn/singlePassApp.hpp>
#include <tkn/types.hpp>
#include <tkn/ccam.hpp>
#include <tkn/stream.hpp>
#include <tkn/features.hpp>
#include <tkn/glsl.hpp>
#include <tkn/shader.hpp>
#include <tkn/render.hpp>
#include <tkn/bits.hpp>
#include <tkn/sh.hpp>
#include <tkn/scene/environment.hpp>

#include <vpp/handles.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/queue.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/submit.hpp>
#include <vpp/image.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/formats.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/util/file.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <shaders/tkn.fullscreen.vert.h>

using namespace tkn::types;
using tkn::glsl::fract;

// sources used
// [1] Physically Based [...] cloud rendering in frostbite, S. Hillaire
//  https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf
// [2] The real-time volumetric cloudscapes of Horizon: Zero Dawn, A. Schneider
//  http://advances.realtimerendering.com/s2015/The%20Real-time%20Volumetric%20Cloudscapes%20of%20Horizon%20-%20Zero%20Dawn%20-%20ARTR.pdf
// [3] Nubis: Authoring Real-Time Volumetric Cloudscapes, A. Schneider
//  http://advances.realtimerendering.com/s2017/Nubis%20-%20Authoring%20Realtime%20Volumetric%20Cloudscapes%20with%20the%20Decima%20Engine%20-%20Final%20.pdf

unsigned ceilDivide(unsigned num, unsigned denom) {
	return (num + denom - 1) / denom;
}

// from pbr/main.cpp
struct SkyData {
	tkn::SH9<Vec4f> skyRadiance; // cosine lobe not applied yet
	Vec3f sunIrradiance;
	Vec3f sunDir;
};

class CloudsApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;

	static constexpr auto noiseExtent = vk::Extent3D{128, 128, 128};
	static constexpr auto noiseLevels = 5u;
	// static constexpr auto noiseFormat = vk::Format::r8g8b8a8Unorm;
	static constexpr auto noiseFormat = vk::Format::r16g16b16a16Sfloat;
	static constexpr auto groupDimSize = 8u;

	struct UboData {
		Vec3f camPos;
		float aspect;
		Vec3f camDir;
		float fov {0.5 * nytl::constants::pi};
		Vec3f sunDir;
		float time;
		Vec3f sunIrradiance;
	};

public:
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto cctrl = tkn::ControlledCamera::ControlType::firstPerson;
		cam_ = tkn::ControlledCamera(cctrl);

		auto& dev = vkDevice();

		auto sci = tkn::linearSamplerInfo();
		sci.addressModeU = vk::SamplerAddressMode::repeat;
		sci.addressModeV = vk::SamplerAddressMode::repeat;
		sci.addressModeW = vk::SamplerAddressMode::repeat;
		sci.maxAnisotropy = 1.f;
		sci.anisotropyEnable = false;
		// NOTE: for testing
		// sci.minFilter = vk::Filter::nearest;
		// sci.magFilter = vk::Filter::nearest;
		// sci.mipmapMode = vk::SamplerMipmapMode::nearest;
		sampler_ = {dev, sci};

		// init noise tex
		vk::ImageCreateInfo ici;
		ici.extent = noiseExtent;
		ici.format = noiseFormat;
		ici.imageType = vk::ImageType::e3d;
		ici.initialLayout = vk::ImageLayout::undefined;
		ici.mipLevels = noiseLevels;
		ici.arrayLayers = 1u;
		ici.tiling = vk::ImageTiling::optimal;
		ici.sharingMode = vk::SharingMode::exclusive;
		ici.samples = vk::SampleCountBits::e1;
		ici.usage = vk::ImageUsageBits::sampled |
			vk::ImageUsageBits::storage |
			// transfer mainly needed for mipmapping
			vk::ImageUsageBits::transferSrc |
			vk::ImageUsageBits::transferDst;


		noiseImage_ = {dev.devMemAllocator(), ici};

		vk::ImageViewCreateInfo ivi;
		ivi.image = noiseImage_;
		ivi.format = noiseFormat;
		ivi.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		ivi.viewType = vk::ImageViewType::e3d;
		gen_.noiseView = {dev, ivi};

		ivi.subresourceRange.levelCount = noiseLevels;
		render_.noiseView = {dev, ivi};

		if(!initGfx()) {
			return false;
		}

		initSky();

		// init noise generation data
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute),
		};

		gen_.dsLayout = {dev, bindings};
		gen_.pipeLayout = {dev, {{gen_.dsLayout.vkHandle()}}, {}};
		gen_.sem = {dev};

		gen_.ds = {dev.descriptorAllocator(), gen_.dsLayout};
		vpp::DescriptorSetUpdate dsu(gen_.ds);
		dsu.storage({{{}, gen_.noiseView, vk::ImageLayout::general}});
		dsu.apply();

		auto qfam = dev.queueSubmitter().queue().family();
		gen_.cb = dev.commandAllocator().get(qfam, vk::CommandPoolCreateBits::resetCommandBuffer);

		if(!loadNoiseGen()) {
			return false;
		}

		return true;
	}

	bool initGfx() {
		auto& dev = vkDevice();
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment | vk::ShaderStageBits::vertex),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		render_.dsLayout = {dev, bindings};
		render_.pipeLayout = {dev, {{render_.dsLayout.vkHandle()}}, {}};

		render_.ubo = {dev.bufferAllocator(), sizeof(UboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		render_.ds = {dev.descriptorAllocator(), render_.dsLayout};
		vpp::DescriptorSetUpdate dsu(render_.ds);
		dsu.uniform({{{render_.ubo}}});
		dsu.imageSampler({{{}, render_.noiseView,
			vk::ImageLayout::shaderReadOnlyOptimal}});

		return loadGfxPipe();
	}

	void initSky() {
		auto& dev = vkDevice();

		tkn::SkyboxRenderer::PipeInfo pi;
		pi.sampler = sampler_;
		pi.camDsLayout = render_.dsLayout;
		pi.renderPass = renderPass();
		pi.layered = true;
		// NOTE: technically, this is highly incorrect since we
		// blend the result with the clouds. But i'm too lazy to
		// do a post-process pass now that does all the tonemapping
		// etc so we just do it here; not that much of a difference
		// in practice. what you gonna do
		pi.tonemap = true;
		sky_.renderer.create(dev, pi);

		// must be generated by pbr (see pbr/main.cpp; --bakesky)
		auto data = vpp::readFile("skyData.bin");
		if(data.empty()) {
			throw std::runtime_error("skyData.bin not found");
		}

		auto span = tkn::bytes(data);
		dlg_assert(data.size() % sizeof(SkyData) == 0u);
		auto steps = data.size() / sizeof(SkyData);

		sky_.data.reserve(steps);
		for(auto i = 0u; i < steps; ++i) {
			sky_.data.push_back(tkn::read<SkyData>(span));
		}

		std::unique_ptr<tkn::ImageProvider> cubemaps;
		auto stream = std::make_unique<tkn::FileStream>(tkn::File("skyEnvs.ktx", "rb"));
		auto err = tkn::readKtx(std::move(stream), cubemaps);
		if(err != tkn::ReadError::none) {
			throw std::runtime_error("Couldn't read skyEnvs.ktx");
		}

		dlg_assert(cubemaps->layers() == 6u * steps);

		auto tp = tkn::TextureCreateParams {};
		tp.cubemap = true;
		tp.format = vk::Format::r16g16b16a16Sfloat;
		sky_.cubemaps = buildTexture(dev, std::move(cubemaps), tp);

		sky_.cubemapDs = {dev.descriptorAllocator(), sky_.renderer.dsLayout()};
		sky_.uboDs = {dev.descriptorAllocator(), render_.dsLayout};

		sky_.ubo = {dev.bufferAllocator(), sizeof(nytl::Mat4f) + sizeof(float) * 2,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		vpp::DescriptorSetUpdate dsu1(sky_.cubemapDs);
		dsu1.imageSampler({{{}, sky_.cubemaps.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}});

		vpp::DescriptorSetUpdate dsu2(sky_.uboDs);
		dsu2.uniform({{{sky_.ubo}}});

		vpp::apply({{dsu1, dsu2}});
	}

	bool loadGfxPipe() {
		auto& dev = vkDevice();
		auto mod = tkn::loadShader(dev, "clouds/clouds.frag");
		if(!mod) {
			dlg_error("Failed to reload clouds.frag");
			return false;
		}

		vpp::ShaderModule vert(dev, tkn_fullscreen_vert_data);
		vpp::GraphicsPipelineInfo gpi{renderPass(), render_.pipeLayout, {{{
			{vert, vk::ShaderStageBits::vertex},
			{*mod, vk::ShaderStageBits::fragment},
		}}}};

		gpi.assembly.topology = vk::PrimitiveTopology::triangleFan;
		gpi.blend.pAttachments = &tkn::defaultBlendAttachment();

		render_.pipe = {dev, gpi.info()};
		return true;
	}

	bool loadNoiseGen() {
		auto& dev = vkDevice();
		auto mod = tkn::loadShader(dev, "clouds/genNoise.comp");
		if(!mod) {
			dlg_error("Failed to reload genNoise.comp");
			return false;
		}

		tkn::ComputeGroupSizeSpec spec(groupDimSize, groupDimSize, groupDimSize);

		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = gen_.pipeLayout;
		cpi.stage.module = *mod;
		cpi.stage.pName = "main";
		cpi.stage.pSpecializationInfo = &spec.spec;
		cpi.stage.stage = vk::ShaderStageBits::compute;

		gen_.pipe = {dev, cpi};

		vk::beginCommandBuffer(gen_.cb, {});

		// bring image into general layout
		// we can discard previous contents
		vk::ImageMemoryBarrier imb;
		imb.image = noiseImage_;
		imb.oldLayout = vk::ImageLayout::undefined;
		imb.newLayout = vk::ImageLayout::general;
		imb.srcAccessMask = {};
		imb.dstAccessMask = vk::AccessBits::shaderWrite;
		imb.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(gen_.cb, vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::computeShader, {}, {}, {}, {{imb}});

		// generate noise
		tkn::cmdBindComputeDescriptors(gen_.cb, gen_.pipeLayout, 0,
			{gen_.ds.vkHandle()});
		vk::cmdBindPipeline(gen_.cb, vk::PipelineBindPoint::compute, gen_.pipe);
		vk::cmdDispatch(gen_.cb,
			ceilDivide(noiseExtent.width, groupDimSize),
			ceilDivide(noiseExtent.height, groupDimSize),
			ceilDivide(noiseExtent.depth, groupDimSize));

		// generate LODs. We need this to approximate the rough
		// neighborhood density for lighting.
		tkn::DownscaleTarget dt;
		dt.image = noiseImage_;
		dt.format = noiseFormat;
		dt.extent = noiseExtent;
		dt.srcScope.access = vk::AccessBits::shaderWrite;
		dt.srcScope.layout = vk::ImageLayout::general;
		dt.srcScope.stages = vk::PipelineStageBits::computeShader;

		tkn::SyncScope dst;
		dst.access = vk::AccessBits::shaderRead;
		dst.layout = vk::ImageLayout::shaderReadOnlyOptimal;
		dst.stages = vk::PipelineStageBits::fragmentShader;

		tkn::downscale(gen_.cb, dt, noiseLevels - 1, &dst);
		vk::endCommandBuffer(gen_.cb);

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		tkn::cmdBindGraphicsDescriptors(cb, sky_.renderer.pipeLayout(), 0,
			{sky_.uboDs.vkHandle()});
		sky_.renderer.render(cb, sky_.cubemapDs);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, render_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, render_.pipeLayout, 0,
			{render_.ds.vkHandle()});
		vk::cmdDraw(cb, 4, 1, 0, 0);
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();

		time_ += dt;
		cam_.update(swaDisplay(), dt);

		if(sky_.animate) {
			sky_.time = std::fmod(sky_.time + 0.02 * dt, 1.0);
		}
	}

	void updateDevice() override {
		Base::updateDevice();

		if(render_.reload) {
			loadGfxPipe();
			Base::scheduleRerecord();
			render_.reload = false;
		}

		if(gen_.redo) {
			auto& qs = vkDevice().queueSubmitter();
			vk::SubmitInfo si;
			si.commandBufferCount = 1u;
			si.pCommandBuffers = &gen_.cb.vkHandle();
			si.pSignalSemaphores = &gen_.sem.vkHandle();
			si.signalSemaphoreCount = 1u;
			qs.add(si);

			addSemaphore(gen_.sem, vk::PipelineStageBits::fragmentShader);
			gen_.redo = false;
		}

		{
			auto map = render_.ubo.memoryMap();
			auto span = map.span();

			float gt = sky_.time * sky_.data.size();
			auto& sky0 = sky_.data[std::floor(gt)];
			auto& sky1 = sky_.data[std::ceil(gt)];
			float fac = fract(gt);

			UboData data;
			data.aspect = float(windowSize().x) / windowSize().y;
			data.camDir = cam_.dir();
			data.camPos = cam_.position();
			data.sunDir = nytl::mix(sky0.sunDir, sky1.sunDir, fac);
			data.sunIrradiance = nytl::mix(sky0.sunIrradiance, sky1.sunIrradiance, fac);
			data.time = time_;
			tkn::write(span, data);
			map.flush();
		}

		{
			auto map = sky_.ubo.memoryMap();
			auto span = map.span();
			tkn::write(span, cam_.fixedViewProjectionMatrix());
			tkn::write(span, sky_.time * sky_.data.size());
			tkn::write(span, 3.f); // exposure
			map.flush();
		}
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return false;
		}

		if(!ev.pressed) {
			return false;
		}

		switch(ev.keycode) {
			case swa_key_t:
				if(gen_.redo) {
					dlg_warn("noise rebaking already pending");
					return true;
				}

				// only rebake if shader loading was succesful
				gen_.redo = loadNoiseGen();
				return true;
			case swa_key_r:
				if(render_.reload) {
					dlg_warn("noise rebaking already pending");
					return true;
				}

				render_.reload = true;
				return true;
			case swa_key_p:
				sky_.animate ^= true;
				return true;
			case swa_key_up:
				sky_.time = std::fmod(sky_.time + 0.01, 1.0);
				return true;
			case swa_key_down:
				sky_.time = std::fmod(sky_.time + 0.99, 1.0);
				return false;
			default:
				break;
		}

		return false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		cam_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool features(tkn::Features& enable, const tkn::Features& supported) override {
		if(!supported.base.features.imageCubeArray) {
			dlg_fatal("Required feature 'imageCubeArray' not supported");
			return false;
		}

		enable.base.features.imageCubeArray = true;
		return true;
	}

	bool needsDepth() const override { return false; }
	const char* name() const override { return "clouds"; }

protected:
	tkn::ControlledCamera cam_;
	vpp::Image noiseImage_;
	vpp::Sampler sampler_;
	float time_ {};

	struct {
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDs ds;
		vpp::Pipeline pipe;
		vpp::SubBuffer ubo;
		bool reload {false};
		vpp::ImageView noiseView; // all levels
	} render_;

	// noise texture generation
	struct {
		vpp::Semaphore sem;
		vpp::CommandBuffer cb;
		vpp::TrDsLayout dsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::TrDs ds;
		vpp::Pipeline pipe;
		bool redo {true};
		vpp::ImageView noiseView; // only first level
	} gen_;

	struct {
		std::vector<SkyData> data;
		tkn::SkyboxRenderer renderer;
		vpp::ViewableImage cubemaps;
		vpp::TrDs cubemapDs;
		vpp::TrDs uboDs;
		vpp::SubBuffer ubo;
		bool animate {};
		float time {}; // in range [0, 1], normalized to number of steps
	} sky_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<CloudsApp>(argc, argv);
}
