// TODO
// - add terrain...
// - maybe make the terrain planet-like and allow to leave atmosphere
// - add tesselation shader
//   evaluate terrain generation in tesselation shader?
//   or use PN triangles?
//   use switch it all to a texture-based terrain generation?
//   that has advantages (performance i guess?) but might introduce
//   a (for good quality potentially huge) memory overhead
//   	- lookup the paper of just using a single triangle/quad for terrain
// - efficient culling

#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/render.hpp>
#include <tkn/qcamera.hpp>
#include <tkn/bits.hpp>
#include <tkn/glsl.hpp>
#include <shaders/atmosphere.sky.vert.h>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <ny/mouseButton.hpp>
#include <rvg/context.hpp>
#include <ny/appContext.hpp>
#include <ny/key.hpp>
#include <nytl/mat.hpp>
#include <nytl/vec.hpp>

#ifdef __ANDROID__
#include <shaders/atmosphere.sky.frag.h>
#endif

using tkn::glsl::fract;

class TerrainApp : public tkn::App {
public:
	static constexpr float cameraPosMult = 10000;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		auto& dev = vulkanDevice();

		// layouts
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
		};

		dsLayout_ = {dev, bindings};
		pipeLayout_ = {dev, {{dsLayout_}}, {}};

		// pipeline
		skyVert_ = {dev, atmosphere_sky_vert_data};
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
		dsu.uniform({{{ubo_}}});

		// initial camera pos
		const float planetRadius = 6300000;
		nytl::Vec3f cpos = (planetRadius + 10) * nytl::Vec3f{0, 1, 0};
		camera_.pos = cpos;

		// touch_.positionMultiplier = 0.2 * cameraPosMult;
		// tkn::init(touch_, camera_, rvgContext());

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
		pipe_ = {vulkanDevice(), gpi.info()};
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
		App::update(dt);
		if(playing_) {
			time_ = fract(time_ - 0.05 * dt);
			camera_.update = true; // write ubo
		}

		// tkn::update(touch_, dt);
		tkn::QuatCameraMovement movement;
		movement.fastMult = 500.f;
		movement.slowMult = 50.f;
		checkMovement(camera_, *appContext().keyboardContext(),
			cameraPosMult * dt, movement);

		if(rotateView_) {
			auto sign = [](auto f) { return f > 0.f ? 1.f : -1.f; };
			auto delta = mpos_ - mposStart_;
			vel_.yaw += dt * sign(delta.x) * std::pow(std::abs(delta.x), 1.2);
			vel_.pitch += dt * sign(delta.y) * std::pow(std::abs(delta.y), 1.2);
		}

		// make it really stiff
		vel_.pitch *= std::pow(0.001, dt);
		vel_.yaw *= std::pow(0.001, dt);
		tkn::rotateView(camera_, vel_.yaw, vel_.pitch, 0.f);

		if(camera_.update) {
			App::scheduleRedraw();
		}
	}

	void updateDevice() override {
		App::updateDevice();

#ifndef __ANDROID__
		if(reload_) {
			reload_ = false;
			auto fragMod = tkn::loadShader(device(), "atmosphere/sky.frag");
			if(!fragMod) {
				dlg_error("Failed to reload shader");
			} else {
				createPipeline(*fragMod);
				App::scheduleRerecord();
			}
		}
#endif

		if(camera_.update) {
			auto fov = 0.3 * nytl::constants::pi;
			auto aspect = float(window().size().x) / window().size().y;
			auto near = 0.01f;
			auto far = 30.f;

			camera_.update = false;
			auto map = ubo_.memoryMap();
			auto span = map.span();
			// tkn::write(span, fixedMatrix(camera_));
			auto V = fixedViewMatrix(camera_);
			auto P = tkn::perspective3RH<float>(fov, aspect, near, far);
			tkn::write(span, P * V);
			tkn::write(span, camera_.pos);
			tkn::write(span, time_);
			map.flush();
		}
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		// if(rotateView_) {
		// 	auto x = 0.005 * ev.delta.x, y = 0.005 * ev.delta.y;
		// 	tkn::rotateView(camera_, x, y, 0.f);
		// 	App::scheduleRedraw();
		// }

		using namespace nytl::vec::cw::operators;
		mpos_ = nytl::Vec2f(ev.position) / window().size();
	}

	bool mouseWheel(const ny::MouseWheelEvent& ev) override {
		if(App::mouseWheel(ev)) {
			return true;
		}

		tkn::rotateView(camera_, 0.f, 0.f, 0.1 * ev.value.x);
		return true;
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		using namespace nytl::vec::cw::operators;
		auto mpos = nytl::Vec2f(ev.position) / window().size();
		if(ev.button == ny::MouseButton::left) {
			rotateView_ = ev.pressed;
			mposStart_ = mpos;
			return true;
		}

		return false;
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == ny::Keycode::r) {
#ifndef __ANDROID__
			reload_ = true;
			App::scheduleRedraw();
			return true;
#endif
		} else if(ev.keycode == ny::Keycode::up) {
			time_ = fract(time_ + 0.0025);
			dlg_info("time: {}", time_);
			camera_.update = true; // write ubo
			App::scheduleRedraw();
			return true;
		} else if(ev.keycode == ny::Keycode::down) {
			time_ = fract(time_ - 0.0025);
			dlg_info("time: {}", time_);
			camera_.update = true; // write ubo
			App::scheduleRedraw();
			return true;
		} else if(ev.keycode == ny::Keycode::p) {
			playing_ = !playing_;
			return true;
		}

		return false;
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		// camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	bool touchBegin(const ny::TouchBeginEvent& ev) override {
		if(App::touchBegin(ev)) {
			return true;
		}

		using namespace nytl::vec::cw::operators;
		auto mpos = ev.pos / window().size();
		if(mpos.x > 0.9 && mpos.y > 0.95) {
			playing_ = !playing_;
			return true;
		}

		// tkn::touchBegin(touch_, ev, window().size());
		return true;
	}

	void touchUpdate(const ny::TouchUpdateEvent& ev) override {
		// tkn::touchUpdate(touch_, ev);
		App::scheduleRedraw();
	}

	bool touchEnd(const ny::TouchEndEvent& ev) override {
		if(App::touchEnd(ev)) {
			return true;
		}

		// tkn::touchEnd(touch_, ev);
		App::scheduleRedraw();
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

	bool rotateView_ {};
	nytl::Vec2f mposStart_ {};
	nytl::Vec2f mpos_ {};

	struct {
		float pitch {0.f};
		float yaw {0.f};
	} vel_;

	// tkn::Camera camera_;
	tkn::QuatCamera camera_;

	// tkn::TouchCameraController touch_;

	// vpp::SubBuffer vertices_;
	// vpp::SubBuffer indices_;
	// nytl::Vec2ui size_ {128, 128};
};

int main(int argc, const char** argv) {
	TerrainApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
