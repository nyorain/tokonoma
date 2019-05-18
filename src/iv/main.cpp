#include <stage/app.hpp>
#include <stage/window.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>
#include <stage/defer.hpp>
#include <stage/camera.hpp>

#include <vpp/handles.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/shader.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <ny/key.hpp>
#include <ny/mouseButton.hpp>

#include <argagg.hpp>

#include <shaders/stage.fullscreen.vert.h>
#include <shaders/stage.texture.frag.h>

// TODO: allow selecting different mip layers/faces/array layers
// also add cubemap visualization

class ImageView : public doi::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!doi::App::init(args)) {
			return false;
		}

		auto& dev = device();

		// sampler
		vk::SamplerCreateInfo sci;
		sci.addressModeU = vk::SamplerAddressMode::clampToEdge;
		sci.addressModeV = vk::SamplerAddressMode::clampToEdge;
		sci.addressModeW = vk::SamplerAddressMode::clampToEdge;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.mipmapMode = vk::SamplerMipmapMode::linear;
		sci.minLod = 0.0;
		sci.maxLod = 100.0;
		sci.anisotropyEnable = false;
		sampler_ = {dev, sci};

		// layouts
		auto bindings = {
			vpp::descriptorBinding( // output from combine pass
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		dsLayout_ = {dev, bindings};
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};

		// pipeline
		vpp::ShaderModule vertShader(dev, stage_fullscreen_vert_data);
		vpp::ShaderModule fragShader(dev, stage_texture_frag_data);

		// defaults fit here
		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}});

		pipe_ = {dev, gpi.info()};

		// load image
		image_ = {dev, doi::read(file_)};

		// ds
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.imageSampler({{{{}, image_.imageView(),
			vk::ImageLayout::shaderReadOnlyOptimal}}});
		dsu.apply();

		return true;
	}

	argagg::parser argParser() const override {
		auto parser = App::argParser();
		return parser;
	}

	bool handleArgs(const argagg::parser_results& result) override {
		if(!App::handleArgs(result)) {
			return false;
		}

		if(result.pos.empty()) {
			dlg_fatal("No image argument given");
			return false;
		}

		file_ = result.pos[0];
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {{ds_.vkHandle()}}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0);
	}

	void update(double delta) override {
		App::update(delta);
	}

	/*
	void updateDevice() override {
		// update scene ubo
		if(camera_.update) {
			camera_.update = false;
			// updateLights_ set to false below

			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			doi::write(span, fixedMatrix(camera_));
			doi::write(span, camera_.pos);
		}
	}
	*/

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			doi::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
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

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	const char* name() const override { return "iv"; }

protected:
	std::string file_;
	doi::Texture image_;

	vpp::Sampler sampler_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::Pipeline boxPipe_;

	unsigned layer_ {0};
	unsigned mip_ {0};

	// cubemap
	vpp::SubBuffer cameraUbo_;
	bool rotateView_;
	doi::Camera camera_;
};

int main(int argc, const char** argv) {
	ImageView app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

