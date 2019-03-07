#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/bits.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipelineInfo.hpp>

#include <nytl/mat.hpp>
#include <ny/mouseButton.hpp>
#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>

#include <shaders/fullscreen.vert.h>
#include <shaders/sen.frag.h>
#include "geometry.hpp"

class SenApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto mem = dev.hostMemoryTypes();

		// TODO: use real size instead of larger dummy
		ubo_ = {dev.bufferAllocator(), sizeof(float) * 128,
			vk::BufferUsageBits::uniformBuffer, 0, mem};

		auto renderBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		dsLayout_ = {dev, renderBindings};
		auto pipeSets = {dsLayout_.vkHandle()};

		vk::PipelineLayoutCreateInfo plInfo;
		plInfo.setLayoutCount = 1;
		plInfo.pSetLayouts = pipeSets.begin();
		pipeLayout_ = {dev, {dsLayout_}, {{vk::ShaderStageBits::fragment, 0, 4u}}};

		vpp::ShaderModule fullscreenShader(dev, fullscreen_vert_data);
		vpp::ShaderModule textureShader(dev, sen_frag_data);
		auto rp = renderer().renderPass();
		vpp::GraphicsPipelineInfo pipeInfo(rp, pipeLayout_, {{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}}, 0, renderer().samples());

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},  1, pipeInfo.info(),
			nullptr, vkpipe);
		pipe_ = {dev, vkpipe};

		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		dsu.apply();

		return true;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			using nytl::constants::pi;
			yaw_ += 0.005 * ev.delta.x;
			pitch_ += 0.005 * ev.delta.y;
			pitch_ = std::clamp<float>(pitch_, -pi / 2 + 0.1, pi / 2 - 0.1);

			camera_.dir.x = std::sin(yaw_) * std::cos(pitch_);
			camera_.dir.y = -std::sin(pitch_);
			camera_.dir.z = -std::cos(yaw_) * std::cos(pitch_);
			nytl::normalize(camera_.dir);
			camera_.update = true;
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

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {ds_.vkHandle()}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0);
	}

	void update(double delta) override {
		App::update(delta);
		App::redraw(); // TODO: can be optimized

		// movement
		auto kc = appContext().keyboardContext();
		auto fac = delta;

		auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
		auto right = nytl::normalized(nytl::cross(camera_.dir, yUp));
		auto up = nytl::normalized(nytl::cross(camera_.dir, right));
		if(kc->pressed(ny::Keycode::d)) { // right
			camera_.pos += fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::a)) { // left
			camera_.pos += -fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::w)) {
			camera_.pos += fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::s)) {
			camera_.pos += -fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::q)) { // up
			camera_.pos += -fac * up;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::e)) { // down
			camera_.pos += fac * up;
			camera_.update = true;
		}
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.update) {
			camera_.update = false;
			auto map = ubo_.memoryMap();
			auto span = map.span();

			doi::write(span, camera_.pos);
			doi::write(span, 0.f); // padding
			doi::write(span, camera_.dir);
			doi::write(span, 0.f); // padding
			doi::write(span, fov_);
			doi::write(span, aspect_);
			doi::write(span, float(window().size().x));
			doi::write(span, float(window().size().y));
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		aspect_ = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

private:
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::Pipeline pipe_;

	vpp::TrDs ds_;
	vpp::SubBuffer ubo_;

	struct {
		nytl::Vec3f pos {0.f, 0.f, 0.f};
		nytl::Vec3f dir {0.f, 0.f, -1.f};
		nytl::Mat4f transform;
		bool update {true};
	} camera_;

	bool rotateView_ {};
	float fov_ {nytl::constants::pi * 0.4};
	float aspect_ {1.f};
	float pitch_ {};
	float yaw_ {};
};

int main(int argc, const char** argv) {
	// XXX: just some small tests...
	Box box {{0.f, 0.f, 0.f}};
	Ray ray {{0.f, 0.f, 4.f}, {0.f, 0.f, 1.f}};
	std::printf("should be -3.f: %f\n", intersect(ray, box)); // doesn't intersect

	ray.dir.z = -1.f;
	std::printf("should be 3.f: %f\n", intersect(ray, box));

	// run app
	SenApp app;
	if(!app.init({"sen", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
