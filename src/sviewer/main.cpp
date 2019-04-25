#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/bits.hpp>
#include <stage/util.hpp>
#include <argagg.hpp>

#include <vpp/device.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/util/file.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>

#include <nytl/stringParam.hpp>
#include <nytl/vecOps.hpp>

#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

#include <shaders/fullscreen.vert.h>

// Specification of the frag shader ubo in ./spec.md

class ViewerApp : public doi::App {
public:
	static constexpr auto cacheFile = "sviewer.cache";

	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto mem = dev.hostMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(float) * 64, // should be 'nough
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
		pipeLayout_ = {dev, {dsLayout_}, {}};
		vertShader_ = {dev, fullscreen_vert_data};

		cache_ = {dev, cacheFile};
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		{
			vpp::DescriptorSetUpdate update(ds_);
			update.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		}

		return initPipe(glsl_);
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if (ev.pressed) {
			if(ev.utf8 == std::string("+\0")) {
				++effect_;
				dlg_info("Effect: {}", effect_);
			} else if(effect_ && ev.utf8 == "-") {
				--effect_;
				dlg_info("Effect: {}", effect_);
			} else if(ev.keycode == ny::Keycode::r) {
				reload_ = true;
			} else {
				// check if its a number
				unsigned num;
				if (doi::stoi(ev.utf8, num)) {
					effect_ = num;
					dlg_info("Effect: {}", effect_);
				} else {
					return false;
				}
			}
		} else {
			return false;
		}

		return true;
	}

	void update(double dt) override {
		time_ += dt;
		App::update(dt);
		App::scheduleRedraw(); // we always redraw; shaders are usually dynamic

		auto kc = appContext().keyboardContext();
		auto fac = dt;
		if(kc->pressed(ny::Keycode::d)) {
			camPos_ += nytl::Vec {fac, 0.f, 0.f};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::a)) {
			camPos_ += nytl::Vec {-fac, 0.f, 0.f};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::w)) {
			camPos_ += nytl::Vec {0.f, 0.f, -fac};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::s)) {
			camPos_ += nytl::Vec {0.f, 0.f, fac};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::q)) { // up
			camPos_ += nytl::Vec {0.f, fac, 0.f};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::e)) { // down
			camPos_ += nytl::Vec {0.f, -fac, 0.f};
			App::scheduleRedraw();
		}
	}

	void updateDevice() override {
		// static constexpr auto align = 0.f;
		auto map = ubo_.memoryMap();
		auto span = map.span();
		doi::write(span, mpos_);
		doi::write(span, time_);
		doi::write(span, effect_);
		doi::write(span, camPos_);

		if(reload_) {
			initPipe(glsl_);
			App::scheduleRerecord();
			reload_ = false;
		}
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {ds_.vkHandle()}, {});
		vk::cmdDraw(cb, 6, 1, 0, 0);
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		using namespace nytl::vec::cw::operators;
		mpos_ = nytl::Vec2f(ev.position) / window().size();
	}

	bool initPipe(std::string_view glslFile) {
		auto name = "sviewer/" + std::string(glslFile);
		name += ".frag";
		if(auto mod = doi::loadShader(vulkanDevice(), name)) {
			initPipe(std::move(*mod));
			return true;
		} else {
			return false;
		}
	}

	void initPipe(vpp::ShaderModule frag) {
		auto& dev = vulkanDevice();
		auto rp = renderer().renderPass();
		vpp::GraphicsPipelineInfo pipeInfo(rp, pipeLayout_, {{
			{vertShader_, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment}
		}});

		auto pipes = vk::createGraphicsPipelines(dev, cache_,  {pipeInfo.info()});
		pipeline_ = {dev, pipes[0]};
		vpp::save(dev, cache_, cacheFile);
	}

	bool handleArgs(const argagg::parser_results& parsed) override {
		App::handleArgs(parsed);

		// try to interpret positional argument as shader
		if(parsed.pos.size() > 1) {
			dlg_fatal("Invalid usage (requires one shader parameter)");
			return false;
		}

		if(parsed.pos.size() == 1) {
			glsl_ = parsed.pos[0];
		}

		return true;
	}

protected:
	const char* glsl_ = "svdummy";
	bool reload_ {};

	// glsl ubo vars
	nytl::Vec2f mpos_ {}; // normalized (0 to 1)
	float time_ {}; // in seconds
	std::uint32_t effect_ {};
	nytl::Vec3f camPos_ {};

	vpp::ShaderModule vertShader_;
	vpp::SubBuffer ubo_;
	vpp::Pipeline pipeline_;
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineCache cache_;
};

int main(int argc, const char** argv) {
	ViewerApp app;
	if(!app.init({"shader-viewer", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}

