// TODO
// - add mie scattering
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
#include <tkn/camera.hpp>
#include <tkn/bits.hpp>
#include <tkn/glsl.hpp>
#include <shaders/terrain.sky.vert.h>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <ny/mouseButton.hpp>
#include <ny/key.hpp>
#include <nytl/mat.hpp>
#include <nytl/vec.hpp>

class TerrainApp : public tkn::App {
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
		skyVert_ = {dev, terrain_sky_vert_data};
		auto fragMod = tkn::loadShader(dev, "terrain/sky.frag");
		if(!fragMod) {
			dlg_error("Failed to load shader");
			return false;
		}

		createPipeline(*fragMod);

		// ubo
		auto uboSize = sizeof(nytl::Mat4f) + sizeof(nytl::Vec3f) + sizeof(float);
		ubo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes()};

		// ds
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{{ubo_}}});

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
	}

	void update(double dt) override {
		App::update(dt);
	}

	void updateDevice() override {
		if(reload_) {
			reload_ = false;
			auto fragMod = tkn::loadShader(device(), "terrain/sky.frag");
			if(!fragMod) {
				dlg_error("Failed to reload shader");
			} else {
				createPipeline(*fragMod);
				App::scheduleRerecord();
			}
		}

		if(camera_.update) {
			camera_.update = false;
			auto map = ubo_.memoryMap();
			auto span = map.span();
			tkn::write(span, fixedMatrix(camera_));
			tkn::write(span, camera_.pos);
			tkn::write(span, time_);
			map.flush();
		}
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			tkn::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
			App::scheduleRedraw();
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			rotateView_ = ev.pressed;
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

		using tkn::glsl::fract;
		if(ev.keycode == ny::Keycode::r) {
			reload_ = true;
			App::scheduleRedraw();
		} else if(ev.keycode == ny::Keycode::up) {
			time_ = fract(time_ + 0.01);
			dlg_info("time: {}", time_);
			camera_.update = true; // write ubo
			App::scheduleRedraw();
		} else if(ev.keycode == ny::Keycode::down) {
			time_ = fract(time_ - 0.01);
			dlg_info("time: {}", time_);
			camera_.update = true; // write ubo
			App::scheduleRedraw();
		}

		return false;
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	const char* name() const override { return "terrain"; }

public:
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::SubBuffer ubo_;

	vpp::ShaderModule skyVert_;
	bool reload_ {false};
	float time_ {0.25f}; // in range [0,1]

	bool rotateView_ {};
	tkn::Camera camera_;

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
