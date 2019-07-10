#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/texture.hpp>
#include <tkn/bits.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>
#include <vpp/device.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipeline.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/normals.texn.frag.h>

class DummyApp : public tkn::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto mem = dev.hostMemoryTypes();
		ubo_ = {dev.bufferAllocator(), sizeof(float) * 4,
			vk::BufferUsageBits::uniformBuffer, mem};

		// make sure to load the normal map in linear rgb space, *not* srgb
		tkn::TextureCreateParams params;
		params.srgb = true;
		diffuse_ = std::move(tkn::Texture(dev,
			tkn::read("../assets/gravel_color.png"), params).viewableImage());

		params.srgb = false;
		normal_ = std::move(tkn::Texture(dev,
			tkn::read("../assets/gravel_normal.png"), params).viewableImage());

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
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}},
			{{{vk::ShaderStageBits::fragment, 0, 4u}}}};

		vpp::ShaderModule fullscreenShader(dev, tkn_fullscreen_vert_data);
		vpp::ShaderModule textureShader(dev, normals_texn_frag_data);
		vpp::GraphicsPipelineInfo pipeInfo(renderPass(), pipeLayout_, {{{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}}});

		pipeline_ = {dev, pipeInfo.info()};

		// descriptor
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		{
			vpp::DescriptorSetUpdate update(ds_);
			update.imageSampler({{{}, diffuse_.vkImageView(),
				vk::ImageLayout::shaderReadOnlyOptimal}});
			update.imageSampler({{{}, normal_.vkImageView(),
				vk::ImageLayout::shaderReadOnlyOptimal}});
			update.uniform({{{ubo_}}});
		}

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {{ds_.vkHandle()}}, {});
		vk::cmdDraw(cb, 4, 1, 0, 0);
	}

	void update(double dt) override {
		App::update(dt);

		auto fac = dt;
		auto kc = appContext().keyboardContext();

		// we don't use a transform matrix here, so origin is top left
		if(kc->pressed(ny::Keycode::d)) {
			lightPos_ += nytl::Vec {fac, 0.f, 0.f};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::a)) {
			lightPos_ += nytl::Vec {-fac, 0.f, 0.f};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::w)) {
			lightPos_ += nytl::Vec {0.f, -fac, 0.f};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::s)) {
			lightPos_ += nytl::Vec {0.f, fac, 0.f};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::q)) {
			lightPos_ += nytl::Vec {0.f, 0.f, fac};
			App::scheduleRedraw();
		}
		if(kc->pressed(ny::Keycode::e)) {
			lightPos_ += nytl::Vec {0.f, 0.f, -fac};
			App::scheduleRedraw();
		}

		time_ += dt;
	}

	void updateDevice() override {
		auto map = ubo_.memoryMap();
		auto span = map.span();
		tkn::write(span, lightPos_);
		tkn::write(span, time_);
	}

	const char* name() const override { return "normals"; }

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
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
