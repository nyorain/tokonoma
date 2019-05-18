#include <stage/app.hpp>
#include <stage/window.hpp>
#include <stage/texture.hpp>
#include <stage/defer.hpp>
#include <vpp/handles.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/shader.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <argagg.hpp>

#include <shaders/stage.fullscreen.vert.h>
#include <shaders/stage.texture.frag.h>

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
		doi::WorkBatcher wb(dev);

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		doi::TextureCreateParams params;
		params.format = vk::Format::r16g16b16a16Sfloat;
		image_ = {wb, file_, params};

		vk::endCommandBuffer(cb);
		auto id = qs.add(cb);
		qs.wait(id);

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

	const char* name() const override { return "iv"; }

protected:
	std::string file_;
	doi::Texture image_;

	vpp::Sampler sampler_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

int main(int argc, const char** argv) {
	ImageView app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

