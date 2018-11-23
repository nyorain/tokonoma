#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/bits.hpp>
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
#include <sys/wait.h>

#include <shaders/fullscreen.vert.h>

class DummyApp : public doi::App {
public:
	static constexpr auto cacheFile = "sviewer.cache";

	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto mem = dev.hostMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(float) * 3,
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

		initPipe(glsl_);
		return true;
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == ny::Keycode::r) {
			reload_ = true;
		} else {
			return false;
		}

		return true;
	}

	void update(double dt) override {
		time_ += dt;
		App::update(dt);
		App::redraw(); // we always redraw; shaders are usually dynamic
	}

	void updateDevice() override {
		auto map = ubo_.memoryMap();
		auto span = map.span();
		doi::write(span, mpos_);
		doi::write(span, time_);

		if(reload_) {
			initPipe(glsl_);
			rerecord();
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

	void initPipe(std::string_view glslFile) {
		// TODO: handle error (return bool?)
		static const auto spv = "sviewer.frag.spv";
		std::string cmd = "glslangValidator -V -o ";
		cmd += spv;
		cmd += " -I../src/shaders/ ../src/shaders/sviewer/";
		cmd += glslFile;
		cmd += ".frag";

		std::fprintf(stderr, ">>> Beginning shader compilation\n");
		int ret = std::system(cmd.c_str());
		if (WEXITSTATUS(ret) != 0) {
			dlg_error("Failed to compile shader");
			return;
		}
		std::fprintf(stderr, ">>> Shader compilation successful\n");

		initPipe({vulkanDevice(), spv});
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
	nytl::Vec2f mpos_ {}; // normalized (0 to 1)
	float time_ {}; // in seconds
	bool reload_ {};

	vpp::ShaderModule vertShader_;
	vpp::SubBuffer ubo_;
	vpp::Pipeline pipeline_;
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineCache cache_;
};

int main(int argc, const char** argv) {
	DummyApp app;
	if(!app.init({"dummy", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}

