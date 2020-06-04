#include <tkn/types.hpp>
#include <tkn/ccam.hpp>
#include <tkn/features.hpp>
#include <tkn/defer.hpp>
#include <tkn/texture.hpp>
#include <tkn/f16.hpp>
#include <tkn/shader.hpp>
#include <tkn/image.hpp>
#include <tkn/render.hpp>
#include <tkn/bits.hpp>
#include <tkn/sky.hpp>
#include <tkn/scene/environment.hpp>
#include <tkn/singlePassApp.hpp>

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

#include <shaders/tkn.skybox.vert.h>

using std::move;
using nytl::constants::pi;
using namespace tkn::types;
using namespace tkn::hosekSky;

class SkyApp : public tkn::SinglePassApp {
public:
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
	using Base = tkn::SinglePassApp;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
		sampler_ = {dev, tkn::linearSamplerInfo()};

		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		dsLayout_ = {dev, bindings};
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
		ubo_ = {dev.bufferAllocator(), sizeof(UboData),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		tables_.semaphore = vpp::Semaphore{dev};

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		tables_.cb = dev.commandAllocator().get(qfam, vk::CommandPoolCreateBits::resetCommandBuffer);
		buildSky();

		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{{ubo_}}});
		dsu.imageSampler({{{}, tables_.f.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, tables_.g.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.imageSampler({{{}, tables_.fhh.imageView(), vk::ImageLayout::shaderReadOnlyOptimal}});
		dsu.apply();

		if(!loadPipe()) {
			return false;
		}

		return true;
	}

	bool loadPipe() {
		auto& dev = vkDevice();
		vpp::ShaderModule vert(dev, tkn_skybox_vert_data);
		auto mod = tkn::loadShader(dev, "sky/sky.frag");
		if(!mod) {
			dlg_error("Failed to reload sky.frag");
			return false;
		}

		vpp::GraphicsPipelineInfo gpi{renderPass(), pipeLayout_, {{{
			{vert, vk::ShaderStageBits::vertex},
			{*mod, vk::ShaderStageBits::fragment},
		}}}};

		gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
		gpi.blend.pAttachments = &tkn::noBlendAttachment();

		pipe_ = {dev, gpi.info()};
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_.vkHandle()});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdDraw(cb, 14, 1, 0, 0); // skybox strip
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();
		camera_.update(swaDisplay(), dt);
	}

	void updateDevice() override {
		Base::updateDevice();

		if(reloadPipe_) {
			reloadPipe_ = false;
			loadPipe();
			Base::scheduleRerecord();
		}

		auto coeff = [&](unsigned i) {
			return Vec3f{
				sky_.config.coeffs[0][i],
				sky_.config.coeffs[1][i],
				sky_.config.coeffs[2][i],
			};
		};

		UboData data;
		data.transform = camera_.fixedViewProjectionMatrix();
		// data.sunColor = sunRadianceRGB(dot(sky_.toSun, Vec3f{0.f, 1.f, 0.f}), sky_.turbidity);
		// data.sunColor = 100'000.f * Vec3f{0.9f, 0.8f, 0.7f};
		data.sunColor = (1 / 0.000068f) * tkn::Sky::sunIrradiance(sky_.turbidity, sky_.groundAlbedo, sky_.toSun);
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
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		camera_.aspect({w, h});
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		float diff = 0.01 / (1.0 + 5 * std::abs(std::sin(2 * pi * daytime_)));
		switch(ev.keycode) {
			case swa_key_r:
				reloadPipe_ = true;
				return true;
			case swa_key_up:
				daytime_ = std::fmod(daytime_ + diff, 1.0);
				dlg_info("daytime: {}", daytime_);
				buildSky();
				return true;
			case swa_key_down:
				daytime_ = std::fmod(daytime_ + 1.0 - diff, 1.0); // -0.0025
				dlg_info("daytime: {}", daytime_);
				buildSky();
				return true;
			case swa_key_left:
				roughness_ = std::clamp(roughness_ - 0.02, 0.0, 1.0);
				break;
			case swa_key_right:
				roughness_ = std::clamp(roughness_ + 0.02, 0.0, 1.0);
				break;
			case swa_key_pageup:
				turbidity_ = std::clamp(turbidity_ * 1.1, 1.0, 10.0);
				dlg_info("turbidity: {}", turbidity_);
				buildSky();
				return true;
			case swa_key_pagedown:
				turbidity_ = std::clamp(turbidity_ / 1.1, 1.0, 10.0);
				dlg_info("turbidity: {}", turbidity_);
				buildSky();
				return true;
			default:
				break;
		}

		return false;
	}

	void buildSky() {
		auto& dev = vkDevice();

		float t = 2 * pi * daytime_;
		auto dir = Vec3f{0.5f * std::sin(t), std::cos(t), 0.f};

		sky_.groundAlbedo = {0.7f, 0.7f, 0.9f};
		sky_.turbidity = turbidity_;
		sky_.toSun = nytl::normalized(dir);
		sky_.config = bakeConfiguration(sky_.turbidity, sky_.groundAlbedo, sky_.toSun);
		auto table = generateTable(sky_);

		auto fmt = vk::Format::r16g16b16a16Sfloat;
		auto size = Vec2ui{Table::numEntries, Table::numLevels};

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

		auto imgF = tkn::wrap(size, fmt, tkn::bytes(dataF));
		auto imgG = tkn::wrap(size, fmt, tkn::bytes(dataG));
		auto imgFHH = tkn::wrap(size, fmt, tkn::bytes(dataFHH));

		vk::CommandBuffer cb = tables_.cb;
		vk::beginCommandBuffer(cb, {});

		tkn::TextureCreateParams tcp;
		tcp.format = fmt;

		std::array<tkn::FillData, 3> datas;
		if(!tables_.f.image()) {
			auto wb = tkn::WorkBatcher::createDefault(dev);
			wb.cb = cb;

			tables_.f = move(tkn::Texture{wb, move(imgF), tcp}.viewableImage());
			tables_.g = move(tkn::Texture{wb, move(imgG), tcp}.viewableImage());
			tables_.fhh = move(tkn::Texture{wb, move(imgFHH), tcp}.viewableImage());
		} else {
			datas[0] = createFill(tables_.f.image(), fmt, std::move(imgF));
			datas[1] = createFill(tables_.g.image(), fmt, std::move(imgG));
			datas[2] = createFill(tables_.fhh.image(), fmt, std::move(imgFHH));

			doFill(datas[0], cb);
			doFill(datas[1], cb);
			doFill(datas[2], cb);
		}

		vk::endCommandBuffer(cb);

		vk::SubmitInfo si;
		si.commandBufferCount = 1u;
		si.pCommandBuffers = &tables_.cb.vkHandle();
		si.pSignalSemaphores = &tables_.semaphore.vkHandle();
		si.signalSemaphoreCount = 1u;

		auto& qs = dev.queueSubmitter();
		qs.add(si);
		Base::addSemaphore(tables_.semaphore, vk::PipelineStageBits::fragmentShader);
	}

	const char* name() const override { return "sky"; }
	bool needsDepth() const override { return false; }

protected:
	tkn::ControlledCamera camera_;

	vpp::Sampler sampler_;
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;

	Sky sky_;
	float daytime_ {};
	float turbidity_ {2.f};
	float roughness_ {0.f};

	bool reloadPipe_ {};
	struct {
		Table table;
		vpp::CommandBuffer cb;
		vpp::Semaphore semaphore;

		vpp::ViewableImage f;
		vpp::ViewableImage g;
		vpp::ViewableImage fhh;
	} tables_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<SkyApp>(argc, argv);
}

