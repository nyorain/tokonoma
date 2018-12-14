#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/texture.hpp>
#include <stage/bits.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/device.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipelineInfo.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>

#include <shaders/fullscreen.vert.h>
#include <shaders/texn.frag.h>

class DummyApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto mem = dev.hostMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(float) * 4,
			vk::BufferUsageBits::uniformBuffer, 0, mem};

		diffuse_ = doi::loadTexture(dev, "../assets/gravel_color.png");
		normal_ = doi::loadTexture(dev, "../assets/gravel_normal.png");

		// pipe
		auto info = vk::SamplerCreateInfo {};
		info.maxAnisotropy = 1.0;
		info.magFilter = vk::Filter::linear;
		info.minFilter = vk::Filter::linear;
		info.minLod = 0;
		info.maxLod = 0.25;
		info.mipmapMode = vk::SamplerMipmapMode::nearest;
		sampler_ = vpp::Sampler(dev, info);

		auto renderBindings = {
			// diffuse + normals
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
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
		vpp::ShaderModule textureShader(dev, texn_frag_data);
		auto rp = renderer().renderPass();
		vpp::GraphicsPipelineInfo pipeInfo(rp, pipeLayout_, {{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}});

		auto pipes = vk::createGraphicsPipelines(dev, {},  {pipeInfo.info()});
		pipeline_ = {dev, pipes[0]};

		// descriptor
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		{
			vpp::DescriptorSetUpdate update(ds_);
			update.imageSampler({{{}, diffuse_.vkImageView(),
				vk::ImageLayout::shaderReadOnlyOptimal}});
			update.imageSampler({{{}, normal_.vkImageView(),
				vk::ImageLayout::shaderReadOnlyOptimal}});
			update.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		}

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {ds_.vkHandle()}, {});
		vk::cmdDraw(cb, 6, 1, 0, 0);
	}

	void update(double dt) override {
		App::update(dt);
		App::redraw();

		auto fac = dt;
		auto kc = appContext().keyboardContext();

		// we don't use a transform matrix here, so origin is top left
		if(kc->pressed(ny::Keycode::d)) {
			lightPos_ += nytl::Vec {fac, 0.f, 0.f};
			App::redraw();
		}
		if(kc->pressed(ny::Keycode::a)) {
			lightPos_ += nytl::Vec {-fac, 0.f, 0.f};
			App::redraw();
		}
		if(kc->pressed(ny::Keycode::w)) {
			lightPos_ += nytl::Vec {0.f, -fac, 0.f};
			App::redraw();
		}
		if(kc->pressed(ny::Keycode::s)) {
			lightPos_ += nytl::Vec {0.f, fac, 0.f};
			App::redraw();
		}
		if(kc->pressed(ny::Keycode::q)) {
			lightPos_ += nytl::Vec {0.f, 0.f, fac};
			App::redraw();
		}
		if(kc->pressed(ny::Keycode::e)) {
			lightPos_ += nytl::Vec {0.f, 0.f, -fac};
			App::redraw();
		}

		time_ += dt;
	}

	void updateDevice() override {
		auto map = ubo_.memoryMap();
		auto span = map.span();
		doi::write(span, lightPos_);
		doi::write(span, time_);
	}

protected:
	float time_ {};
	vpp::Sampler sampler_;
	vpp::SubBuffer ubo_; // light position
	vpp::ViewableImage diffuse_;
	vpp::ViewableImage normal_;
	vpp::Pipeline pipeline_;
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;

	nytl::Vec3f lightPos_ {0.5f, 0.5f, -1.f};
};

int main(int argc, const char** argv) {
	DummyApp app;
	if(!app.init({"dummy", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
