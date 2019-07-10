#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/bits.hpp>
#include <tkn/util.hpp>
#include <argagg.hpp>

#include <vpp/device.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>
#include <vpp/util/file.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>

#include <nytl/stringParam.hpp>
#include <nytl/vecOps.hpp>

#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>

#include <shaders/tkn.fullscreen.vert.h>

// Specification of the frag shader ubo in ./spec.md

class ViewerApp : public tkn::App {
public:
	static constexpr auto cacheFile = "sviewer.cache";

	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto mem = dev.hostMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(float) * 64, // should be 'nough
			vk::BufferUsageBits::uniformBuffer, mem};

		auto renderBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
		};

		dsLayout_ = {dev, renderBindings};
		auto pipeSets = {dsLayout_.vkHandle()};

		vk::PipelineLayoutCreateInfo plInfo;
		plInfo.setLayoutCount = 1;
		plInfo.pSetLayouts = pipeSets.begin();
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};
		vertShader_ = {dev, tkn_fullscreen_vert_data};

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
				if (tkn::stoi(ev.utf8, num)) {
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
		tkn::write(span, mpos_);
		tkn::write(span, time_);
		tkn::write(span, effect_);
		tkn::write(span, camPos_);

		if(reload_) {
			initPipe(glsl_);
			App::scheduleRerecord();
			reload_ = false;
		}
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {{ds_.vkHandle()}}, {});
		vk::cmdDraw(cb, 6, 1, 0, 0);
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		using namespace nytl::vec::cw::operators;
		mpos_ = nytl::Vec2f(ev.position) / window().size();
	}

	bool initPipe(std::string_view glslFile) {
		auto name = "sviewer/shaders/" + std::string(glslFile);
		name += ".frag";
		if(auto mod = tkn::loadShader(vulkanDevice(), name)) {
			initPipe(std::move(*mod));
			return true;
		} else {
			return false;
		}
	}

	void initPipe(vpp::ShaderModule frag) {
		auto& dev = vulkanDevice();
		vpp::GraphicsPipelineInfo pipeInfo(renderPass(), pipeLayout_, {{{
			{vertShader_, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment}
		}}});

		pipeline_ = {dev, pipeInfo.info(), cache_};
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

	const char* name() const override { return "Shader Viewer"; }

protected:
	const char* glsl_ = "dummy";
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
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

