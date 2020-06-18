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

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();

		genSem_ = vpp::Semaphore{dev};
		genCb_ = dev.commandAllocator().get(qfam,
			vk::CommandPoolCreateBits::resetCommandBuffer);

		if(!hosekSky_.init(dev, sampler_, renderPass())) {
			return false;
		}

		if(!brunetonSky_.init(dev, sampler_, renderPass())) {
			return false;
		}

		// position does not matter for hosek sky so we always
		// just use the bruneton sky one.
		camera_.position(brunetonSky_.startViewPos());

		rebuild();
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

		if(rebuild_) {
			rebuild_ = false;
			rebuild();
		}

		auto rerec = false;
		if(useHosek_) {
			rerec |= hosekSky_.updateDevice(camera_, reloadPipe_);
		} else {
			rerec |= brunetonSky_.updateDevice(camera_, reloadPipe_,
				reloadGenPipe_, toSun());

			if(reloadGenPipe_) {
				reloadGenPipe_ = false;
				rebuild();
			}
		}

		reloadPipe_ = false;
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
		if(useHosek_) {
			switch(ev.keycode) {
				case swa_key_left: {
					auto& s = hosekSky_;
					s.roughness(std::clamp(s.roughness() - 0.02, 0.0, 1.0));
					dlg_info("roughness: {}", s.roughness());
					return true;
				} case swa_key_right: {
					auto& s = hosekSky_;
					s.roughness(std::clamp(s.roughness() + 0.02, 0.0, 1.0));
					dlg_info("roughness: {}", s.roughness());
					return true;
				} case swa_key_pageup: {
					auto& s = hosekSky_;
					s.turbidity(std::clamp(s.turbidity() + 0.25, 1.0, 10.0));
					dlg_info("turbidity: {}", s.turbidity());
					rebuild_ = true;
					return true;
				} case swa_key_pagedown: {
					auto& s = hosekSky_;
					s.turbidity(std::clamp(s.turbidity() - 0.25, 1.0, 10.0));
					dlg_info("turbidity: {}", s.turbidity());
					rebuild_ = true;
					return true;
				}
				default: break;
			}
		} else {
			switch(ev.keycode) {
				case swa_key_left: {
					auto order = brunetonSky_.maxScatOrder_;
					order = std::clamp(order - 1u, 1u, 10u);
					dlg_info("maxScatOrder: {}", order);
					brunetonSky_.maxScatOrder_ = order;
					rebuild_ = true;
					return true;
				} case swa_key_right: {
					auto order = brunetonSky_.maxScatOrder_;
					order = std::clamp(order + 1u, 1u, 10u);
					dlg_info("maxScatOrder: {}", order);
					brunetonSky_.maxScatOrder_ = order;
					rebuild_ = true;
					return true;
				} case swa_key_t:
					reloadGenPipe_ = true;
					return true;
				default:
					break;
			}
		}

		switch(ev.keycode) {
			case swa_key_r:
				reloadPipe_ = true;
				return true;
			case swa_key_up:
				daytime_ = std::fmod(daytime_ + diff, 1.0);
				dlg_info("daytime: {}", daytime_);
				if(useHosek_) {
					rebuild_ = true;
				}
				return true;
			case swa_key_down:
				daytime_ = std::fmod(daytime_ + 1.0 - diff, 1.0); // -0.0025
				dlg_info("daytime: {}", daytime_);
				if(useHosek_) {
					rebuild_ = true;
				}
				return true;
			case swa_key_o:
				switchSky();
				return true;
			default:
				break;
		}

		return false;
	}

	void switchSky() {
		useHosek_ = !useHosek_;
		dlg_info("useHosek: {}", useHosek_);
		rebuild_ = true;
		Base::scheduleRerecord();

		if(useHosek_) {
			camera_.useFirstPersonControl();
		} else {
			camera_.useSpaceshipControl();
			auto& c = **camera_.spaceshipControl();
			c.controls.move.mult = 10000.f;
			c.controls.move.fastMult = 100.f;
			c.controls.move.slowMult = 0.05f;
		}
	}

	void rebuild() {
		vk::beginCommandBuffer(genCb_, {});

		if(useHosek_) {
			hosekSky_.buildTables(genCb_, toSun());
		} else {
			// TODO; we don't need to record this every time,
			// the command buffer stays the same for bruneton.
			// Only needs to be done after reloading the pipe
			brunetonSky_.generate(genCb_);
		}

		vk::endCommandBuffer(genCb_);
		auto& qs = vkDevice().queueSubmitter();

		vk::SubmitInfo si;
		si.commandBufferCount = 1u;
		si.pCommandBuffers = &genCb_.vkHandle();
		si.pSignalSemaphores = &genSem_.vkHandle();
		si.signalSemaphoreCount = 1u;
		qs.add(si);

		Base::addSemaphore(genSem_, vk::PipelineStageBits::allGraphics);
	}

	const char* name() const override { return "sky"; }
	bool needsDepth() const override { return false; }

private:
	tkn::ControlledCamera camera_;

	bool rebuild_ {false};
	bool useHosek_ {true};
	HosekWilkieSky hosekSky_;
	BrunetonSky brunetonSky_;
	vpp::Sampler sampler_;

	vpp::CommandBuffer genCb_; // for generating/uploading data
	vpp::Semaphore genSem_;

	float daytime_ {};
	bool reloadPipe_ {};
	bool reloadGenPipe_ {};
};

int main(int argc, const char** argv) {
	return tkn::appMain<SkyApp>(argc, argv);
}

