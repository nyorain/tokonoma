#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/formats.hpp>
#include <vpp/image.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/pipelineInfo.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <ny/key.hpp>
#include <dlg/dlg.hpp>
#include <cstring>
#include <random>

#include <shaders/fullscreen.vert.h>
#include <shaders/texture.frag.h>
#include <shaders/advect.vel.comp.h>
#include <shaders/advect.dens.comp.h>

// == FluidSystem ==
class FluidSystem {
public:
	FluidSystem(vpp::Device& dev, nytl::Vec2ui size);

	void updateDevice(float dt);
	void compute(vk::CommandBuffer);

	const auto& density() const { return density_; }
	const auto& velocity() const { return velocity_; }

protected:
	nytl::Vec2ui size_;
	vpp::Sampler sampler_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs advectDensityDs_;
	vpp::TrDs advectVelocityDs_;
	vpp::PipelineLayout pipeLayout_;
	vpp::SubBuffer ubo_;

	vpp::Pipeline advectVel_;
	vpp::Pipeline advectDens_;

	vpp::ViewableImage velocity_;
	vpp::ViewableImage velocity0_;

	vpp::ViewableImage density_;
	vpp::ViewableImage density0_;
};

FluidSystem::FluidSystem(vpp::Device& dev, nytl::Vec2ui size) {
	size_ = size;

	// sampler
	auto info = vk::SamplerCreateInfo {};
	info.maxAnisotropy = 1.0;
	info.magFilter = vk::Filter::linear;
	info.minFilter = vk::Filter::linear;
	info.minLod = 0;
	info.maxLod = 0.25;
	info.mipmapMode = vk::SamplerMipmapMode::nearest;
	sampler_ = vpp::Sampler(dev, info);

	// pipe
	auto advectBindings = {
		vpp::descriptorBinding(vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::compute, -1, 1, &sampler_.vkHandle()),
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::compute)
	};

	dsLayout_ = {dev, advectBindings};
	auto pipeSets = {dsLayout_.vkHandle()};

	vk::PipelineLayoutCreateInfo plInfo;
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = pipeSets.begin();
	pipeLayout_ = {dev, plInfo};

	auto advectVelShader = vpp::ShaderModule(dev, advect_vel_comp_data);
	auto advectDensShader = vpp::ShaderModule(dev, advect_dens_comp_data);

	vk::ComputePipelineCreateInfo advectInfoVel;
	advectInfoVel.layout = pipeLayout_;
	advectInfoVel.stage.module = advectVelShader;
	advectInfoVel.stage.pName = "main";
	advectInfoVel.stage.stage = vk::ShaderStageBits::compute;

	auto advectInfoDens = advectInfoVel;
	advectInfoDens.stage.module = advectDensShader;

	auto pipes = vk::createComputePipelines(dev, {}, {
		advectInfoVel, advectInfoDens});
	advectVel_ = {dev, pipes[0]};
	advectDens_ = {dev, pipes[1]};

	// images
	auto extent = vk::Extent3D {size.x, size.y, 1};
	auto usage = vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::storage |
		vk::ImageUsageBits::transferDst;
	auto velInfo = vpp::ViewableImageCreateInfo::color(dev, extent,
		usage, {vk::Format::r16g16b16a16Sfloat}).value();
	velocity_ = {dev, velInfo};
	velocity0_ = {dev, velInfo};

	auto densInfo = vpp::ViewableImageCreateInfo::color(dev, extent,
		usage, {vk::Format::r32Sfloat}).value();
	density_ = {dev, densInfo};
	density0_ = {dev, densInfo};

	auto fam = dev.queueSubmitter().queue().family();
	auto cmdBuf = dev.commandAllocator().get(fam);
	auto layout = vk::ImageLayout::undefined;

	vk::beginCommandBuffer(cmdBuf, {});
	for(auto img : {&density_, &density0_, &velocity_, &velocity0_}) {
		vpp::changeLayout(cmdBuf, img->vkImage(),
			layout, vk::PipelineStageBits::topOfPipe, {},
			vk::ImageLayout::general, vk::PipelineStageBits::transfer,
			vk::AccessBits::transferWrite,
			{vk::ImageAspectBits::color, 0, 1, 0, 1});

		auto color = vk::ClearColorValue {{0.f, 0.f, 0.f, 0.f}};
		vk::cmdClearColorImage(cmdBuf, img->vkImage(), vk::ImageLayout::general,
			color, {{vk::ImageAspectBits::color, 0, 1, 0, 1}});
	}
	vk::endCommandBuffer(cmdBuf);

	vk::SubmitInfo subInfo;
	subInfo.commandBufferCount = 1;
	subInfo.pCommandBuffers = &cmdBuf.vkHandle();
	dev.queueSubmitter().add(subInfo);
	dev.queueSubmitter().submit();
	vk::deviceWaitIdle(dev);

	// ds & stuff
	ubo_ = {dev.bufferAllocator(), 4u, vk::BufferUsageBits::uniformBuffer,
		4u, dev.hostMemoryTypes()};

	advectDensityDs_ = {dev.descriptorAllocator(), dsLayout_};
	advectVelocityDs_ = {dev.descriptorAllocator(), dsLayout_};

	vpp::DescriptorSetUpdate densUpdate(advectDensityDs_);
	densUpdate.storage({{{}, density_.vkImageView(), vk::ImageLayout::general}});
	densUpdate.imageSampler({{{}, density0_.vkImageView(), vk::ImageLayout::general}});
	densUpdate.imageSampler({{{}, velocity_.vkImageView(), vk::ImageLayout::general}});
	densUpdate.uniform({{ubo_.buffer(), ubo_.offset(), 4u}});

	vpp::DescriptorSetUpdate velUpdate(advectVelocityDs_);
	velUpdate.storage({{{}, velocity_.vkImageView(), vk::ImageLayout::general}});
	velUpdate.imageSampler({{{}, velocity0_.vkImageView(), vk::ImageLayout::general}});
	velUpdate.imageSampler({{{}, velocity0_.vkImageView(), vk::ImageLayout::general}});
	velUpdate.uniform({{ubo_.buffer(), ubo_.offset(), 4u}});
}

void FluidSystem::updateDevice(float dt) {
	auto map = ubo_.memoryMap();
	std::memcpy(map.ptr(), &dt, sizeof(dt));
}

void FluidSystem::compute(vk::CommandBuffer cb) {
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, advectVel_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {advectVelocityDs_}, {});
	vk::cmdDispatch(cb, size_.x, size_.y, 1);

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, advectDens_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {advectDensityDs_}, {});
	vk::cmdDispatch(cb, size_.x, size_.y, 1);
}

// == FluidApp ==
class FluidApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!App::init(settings)) {
			return false;
		}

		auto& dev = vulkanDevice();

		// own pipe
		auto info = vk::SamplerCreateInfo {};
		info.maxAnisotropy = 1.0;
		info.magFilter = vk::Filter::linear;
		info.minFilter = vk::Filter::linear;
		info.minLod = 0;
		info.maxLod = 0.25;
		info.mipmapMode = vk::SamplerMipmapMode::nearest;
		sampler_ = vpp::Sampler(dev, info);

		auto renderBindings = {
			vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle())
		};

		dsLayout_ = {dev, renderBindings};
		auto pipeSets = {dsLayout_.vkHandle()};

		vk::PipelineLayoutCreateInfo plInfo;
		plInfo.setLayoutCount = 1;
		plInfo.pSetLayouts = pipeSets.begin();
		pipeLayout_ = {dev, plInfo};

		vpp::ShaderModule fullscreenShader(dev, fullscreen_vert_data);
		vpp::ShaderModule textureShader(dev, texture_frag_data);
		auto rp = renderer().renderPass();

		vpp::GraphicsPipelineInfo pipeInfo(rp, pipeLayout_, {{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}});

		auto pipes = vk::createGraphicsPipelines(dev, {},  {pipeInfo.info()});
		pipe_ = {dev, pipes[0]};

		// system
		system_.emplace(vulkanDevice(), nytl::Vec2ui {512, 512});

		// ds
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		auto iv = system_->density().vkImageView();
		vpp::DescriptorSetUpdate update(ds_);
		update.imageSampler({{{}, iv, vk::ImageLayout::general}});

		return true;
	}

	void key(const ny::KeyEvent& ev) override {
		App::key(ev);
		if(!ev.pressed) {
			return;
		}

		if(ev.keycode == ny::Keycode::d) {
			auto iv = system_->density().vkImageView();
			vpp::DescriptorSetUpdate update(ds_);
			update.imageSampler({{{}, iv, vk::ImageLayout::general}});
			rerecord();
		} else if(ev.keycode == ny::Keycode::v) {
			auto iv = system_->velocity().vkImageView();
			vpp::DescriptorSetUpdate update(ds_);
			update.imageSampler({{{}, iv, vk::ImageLayout::general}});
			rerecord();
		}
	}

	void update(double dt) override {
		App::update(dt);
		dt_ = dt;
	}

	void updateDevice() override {
		App::updateDevice();
		system_->updateDevice(dt_);
	}

	void beforeRender(vk::CommandBuffer cb) override {
		system_->compute(cb);
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {ds_}, {});
		vk::cmdDraw(cb, 6, 1, 0, 0);
	}

protected:
	std::optional<FluidSystem> system_;
	float dt_ {};

	vpp::Sampler sampler_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

// main
int main(int argc, const char** argv) {
	FluidApp app;
	if(!app.init({"fluids", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
