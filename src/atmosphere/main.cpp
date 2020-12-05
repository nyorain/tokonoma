#include <tkn/singlePassApp.hpp>
#include <tkn/ccam.hpp>
#include <tkn/shader.hpp>
#include <tkn/render.hpp>
#include <tkn/f16.hpp>
#include <tkn/transform.hpp>
#include <tkn/bits.hpp>
#include <tkn/glsl.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <rvg/context.hpp>
#include <nytl/mat.hpp>
#include <nytl/vec.hpp>

#include <shaders/tkn.skybox.vert.h>

#ifdef __ANDROID__
#include <shaders/atmosphere.sky.frag.h>
#endif

using tkn::glsl::fract;

class AtmosphereApp : public tkn::SinglePassApp {
public:
	static constexpr float cameraPosMult = 10000;
	using Base = tkn::SinglePassApp;

public:
	using Base::init;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
		rvgInit();

		// layouts
		auto bindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
		};

		dsLayout_.init(dev, bindings);
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};

		// pipeline
		skyVert_ = {dev, tkn_skybox_vert_data};
#ifdef __ANDROID__
		auto fragMod = vpp::ShaderModule{dev, atmosphere_sky_frag_data};
#else
		auto pfragMod = tkn::loadShader(dev, "atmosphere/sky.frag");
		if(!pfragMod) {
			dlg_error("Failed to load shader");
			return false;
		}

		auto fragMod = std::move(*pfragMod);
#endif

		createPipeline(fragMod);

		// ubo
		auto uboSize = sizeof(nytl::Mat4f) + sizeof(nytl::Vec3f) + sizeof(float);
		ubo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes()};

		// ds
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform(ubo_);

		// initial camera pos
		const float planetRadius = 6300000;
		nytl::Vec3f cpos = (planetRadius + 10) * nytl::Vec3f{0, 1, 0};
		camera_.position(cpos);

		camera_.useSpaceshipControl();
		auto& c = **camera_.spaceshipControl();
		c.controls.move.mult = 10000.f;
		c.controls.move.fastMult = 100.f;
		c.controls.move.slowMult = 0.05f;

		return true;
	}

	void createPipeline(vk::ShaderModule fragMod) {
		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{skyVert_, vk::ShaderStageBits::vertex},
			{fragMod, vk::ShaderStageBits::fragment},
		}}});
		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;
		pipe_ = {vkDevice(), gpi.info()};
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		// vk::cmdDraw(cb, size_.x * size_.y, 1, 0, 0);
		vk::cmdDraw(cb, 14, 1, 0, 0); // magic box via vert shader

		// if(touch_.alt) {
		// 	rvgContext().bindDefaults(cb);
		// 	windowTransform().bind(cb);
		// 	touch_.paint.bind(cb);
		// 	touch_.move.circle.fill(cb);
		// 	touch_.rotate.circle.fill(cb);
		// }
	}

	void update(double dt) override {
		Base::update(dt);
		if(playing_) {
			time_ = fract(time_ - 0.05 * dt);
			camera_.needsUpdate = true; // write ubo
		}

		camera_.update(swaDisplay(), dt);
		if(true || camera_.needsUpdate) {
			Base::scheduleRedraw();
		}
	}

	void updateDevice() override {
		Base::updateDevice();

#ifndef __ANDROID__
		if(reload_) {
			reload_ = false;
			auto fragMod = tkn::loadShader(vkDevice(), "atmosphere/sky.frag");
			if(!fragMod) {
				dlg_error("Failed to reload shader");
			} else {
				createPipeline(*fragMod);
				Base::scheduleRerecord();
			}
		}
#endif

		if(camera_.needsUpdate) {
			camera_.needsUpdate = false;
			auto map = ubo_.memoryMap();
			auto span = map.span();
			tkn::write(span, camera_.fixedViewProjectionMatrix());
			tkn::write(span, camera_.position());
			tkn::write(span, time_);
			map.flush();
		}
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == swa_key_r) {
#ifndef __ANDROID__
			reload_ = true;
			Base::scheduleRedraw();
			return true;
#endif
		} else if(ev.keycode == swa_key_up) {
			time_ = fract(time_ + 0.0025);
			dlg_info("time: {}", time_);
			camera_.needsUpdate = true; // write ubo
			Base::scheduleRedraw();
			return true;
		} else if(ev.keycode == swa_key_down) {
			time_ = fract(time_ - 0.0025);
			dlg_info("time: {}", time_);
			camera_.needsUpdate = true; // write ubo
			Base::scheduleRedraw();
			return true;
		} else if(ev.keycode == swa_key_p) {
			playing_ = !playing_;
			return true;
		}

		return false;
	}

	bool mouseButton(const swa_mouse_button_event& ev) override {
		if(Base::mouseButton(ev)) {
			return true;
		}

		camera_.mouseButton(ev.button, ev.pressed);
		return true;
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		camera_.aspect({w, h});
	}

	bool touchBegin(const swa_touch_event& ev) override {
		if(Base::touchBegin(ev)) {
			return true;
		}

		using namespace nytl::vec::cw::operators;
		auto mpos = nytl::Vec2f{float(ev.x), float(ev.y)} / windowSize();
		if(mpos.x > 0.9 && mpos.y > 0.95) {
			playing_ = !playing_;
			return true;
		}

		// tkn::touchBegin(touch_, ev, window().size());
		return true;
	}

	void touchUpdate(const swa_touch_event& ev) override {
		Base::touchUpdate(ev);
		// tkn::touchUpdate(touch_, ev);
		Base::scheduleRedraw();
	}

	bool touchEnd(unsigned id) override {
		if(Base::touchEnd(id)) {
			return true;
		}

		// tkn::touchEnd(touch_, ev);
		Base::scheduleRedraw();
		return true;
	}

	const char* name() const override { return "atmosphere"; }

public:
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::SubBuffer ubo_;

	vpp::ShaderModule skyVert_;
	bool reload_ {false};
	float time_ {0.25f}; // in range [0,1]
	bool playing_ {false};

	// tkn::Camera camera_;
	tkn::ControlledCamera camera_;

	// tkn::TouchCameraController touch_;

	// vpp::SubBuffer vertices_;
	// vpp::SubBuffer indices_;
	// nytl::Vec2ui size_ {128, 128};
};

int main(int argc, const char** argv) {
	return tkn::appMain<AtmosphereApp>(argc, argv);
}
