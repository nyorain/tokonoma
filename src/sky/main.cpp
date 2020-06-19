#include "hosekWilkie.hpp"
#include "bruneton.hpp"

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

#include <array>
#include <shaders/tkn.skybox.vert.h>

using std::move;
using nytl::constants::pi;
using namespace tkn::types;

class SkyApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
		sampler_ = {dev, tkn::linearSamplerInfo()};

		if(!hosekSky_.init(dev, sampler_, renderPass())) {
			return false;
		}

		if(!brunetonSky_.init(dev, sampler_, renderPass())) {
			return false;
		}

		brunetonSky_.toSun = toSun();
		hosekSky_.toSun(toSun());

		// position does not matter for hosek sky so we always
		// just use the bruneton sky one.
		camera_.position(brunetonSky_.startViewPos());
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		if(useHosek_) {
			hosekSky_.render(cb);
		} else {
			brunetonSky_.render(cb);
		}
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();
		camera_.update(swaDisplay(), dt);
	}

	void updateDevice() override {
		Base::updateDevice();

		auto rerec = false;
		vk::Semaphore waitSem {};
		if(useHosek_) {
			rerec |= hosekSky_.updateDevice(camera_);
			if(hosekSky_.waitSemaphore) {
				waitSem = hosekSky_.waitSemaphore;
				hosekSky_.waitSemaphore = {};
			}
		} else {
			rerec |= brunetonSky_.updateDevice(camera_);
			if(brunetonSky_.waitSemaphore) {
				waitSem = brunetonSky_.waitSemaphore;
				brunetonSky_.waitSemaphore = {};
			}
		}

		if(waitSem) {
			Base::addSemaphore(waitSem, vk::PipelineStageBits::allGraphics);
		}

		if(rerec) {
			Base::scheduleRerecord();
		}
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		camera_.aspect({w, h});
	}

	nytl::Vec3f toSun() const {
		float t = 2 * pi * daytime_;
		return {std::cos(t), std::sin(t), 0.f};
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		float diff = 0.001 + 0.005 * std::abs(std::sin(2 * pi * daytime_));
		switch(ev.keycode) {
			case swa_key_up:
				daytime_ = std::fmod(daytime_ + diff, 1.0);
				dlg_info("daytime: {}", daytime_);
				if(useHosek_) {
					hosekSky_.toSun(toSun());
				} else {
					brunetonSky_.toSun = toSun();
				}
				return true;
			case swa_key_down:
				daytime_ = std::fmod(daytime_ + 1.0 - diff, 1.0); // -0.0025
				dlg_info("daytime: {}", daytime_);
				if(useHosek_) {
					hosekSky_.toSun(toSun());
				} else {
					brunetonSky_.toSun = toSun();
				}
				return true;
			case swa_key_o:
				switchSky();
				return true;
			default:
				break;
		}

		if(useHosek_) {
			return hosekSky_.key(ev.keycode);
		} else {
			return brunetonSky_.key(ev.keycode);
		}
	}

	void switchSky() {
		useHosek_ = !useHosek_;
		dlg_info("useHosek: {}", useHosek_);
		Base::scheduleRerecord();

		if(useHosek_) {
			hosekSky_.toSun(toSun());
			camera_.useFirstPersonControl();
		} else {
			brunetonSky_.toSun = toSun();
			camera_.useSpaceshipControl();
			auto& c = **camera_.spaceshipControl();
			c.controls.move.mult = 10000.f;
			c.controls.move.fastMult = 100.f;
			c.controls.move.slowMult = 0.05f;
		}
	}

	const char* name() const override { return "sky"; }
	bool needsDepth() const override { return false; }

private:
	tkn::ControlledCamera camera_;
	bool useHosek_ {true};
	HosekWilkieSky hosekSky_;
	BrunetonSky brunetonSky_;
	vpp::Sampler sampler_;
	float daytime_ {};
};

int main(int argc, const char** argv) {
	return tkn::appMain<SkyApp>(argc, argv);
}

