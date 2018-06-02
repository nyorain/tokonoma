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
#include <ny/mouseButton.hpp>
#include <dlg/dlg.hpp>
#include <cstring>
#include <random>

#include <shaders/fullscreen.vert.h>
#include <shaders/texture.frag.h>
#include <shaders/advect.vel.comp.h>
#include <shaders/advect.dens.comp.h>
#include <shaders/divergence.comp.h>
#include <shaders/pressure.comp.h>
#include <shaders/project.comp.h>

template<typename T>
void write(nytl::Span<std::byte>& span, T&& data) {
	dlg_assert(span.size() >= sizeof(data));
	std::memcpy(span.data(), &data, sizeof(data));
	span = span.slice(sizeof(data), span.size() - sizeof(data));
}

// == FluidSystem ==
class FluidSystem {
public:
	static constexpr auto pressureIterations = 120u;

	float velocityFac {0.0};
	float densityFac {0.0};
	float radius {10.f};

public:
	FluidSystem(vpp::Device& dev, nytl::Vec2ui size);

	void updateDevice(float dt, nytl::Vec2f mp0, nytl::Vec2f mp1);
	void compute(vk::CommandBuffer);

	const auto& density() const { return density_; }
	const auto& velocity() const { return velocity0_; }
	const auto& pressure() const { return pressure_; }
	const auto& divergence() const { return divergence_; }

protected:
	nytl::Vec2ui size_;

	vpp::Sampler sampler_;
	vpp::SubBuffer ubo_;
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;

	vpp::Pipeline advectVel_;
	vpp::TrDs advectVelocityDs_;

	vpp::Pipeline advectDens_;
	vpp::TrDs advectDensityDs_;

	vpp::Pipeline divergencePipe_;
	vpp::TrDs divergenceDs_;

	vpp::Pipeline pressureIteration_;
	vpp::TrDs pressureDs0_;
	vpp::TrDs pressureDs1_;

	vpp::Pipeline project_;
	vpp::TrDs projectDs_;

	vpp::ViewableImage velocity_;
	vpp::ViewableImage velocity0_;

	vpp::ViewableImage density_;
	vpp::ViewableImage density0_;

	vpp::ViewableImage divergence_;
	vpp::ViewableImage pressure_;
	vpp::ViewableImage pressure0_;
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
		vpp::descriptorBinding(vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
		vpp::descriptorBinding(vk::DescriptorType::storageImage,
			vk::ShaderStageBits::compute),
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
	auto divergenceShader = vpp::ShaderModule(dev, divergence_comp_data);
	auto pressureShader = vpp::ShaderModule(dev, pressure_comp_data);
	auto projectShader = vpp::ShaderModule(dev, project_comp_data);

	vk::ComputePipelineCreateInfo advectInfoVel;
	advectInfoVel.layout = pipeLayout_;
	advectInfoVel.stage.module = advectVelShader;
	advectInfoVel.stage.pName = "main";
	advectInfoVel.stage.stage = vk::ShaderStageBits::compute;

	auto advectInfoDens = advectInfoVel;
	advectInfoDens.stage.module = advectDensShader;

	auto divergenceInfo = advectInfoVel;
	divergenceInfo.stage.module = divergenceShader;

	auto pressureInfo = advectInfoVel;
	pressureInfo.stage.module = pressureShader;

	auto projectInfo = advectInfoVel;
	projectInfo.stage.module = projectShader;

	auto pipes = vk::createComputePipelines(dev, {}, {
		advectInfoVel,
		advectInfoDens,
		divergenceInfo,
		pressureInfo,
		projectInfo});
	advectVel_ = {dev, pipes[0]};
	advectDens_ = {dev, pipes[1]};
	divergencePipe_ = {dev, pipes[2]};
	pressureIteration_ = {dev, pipes[3]};
	project_ = {dev, pipes[4]};

	// images
	auto extent = vk::Extent3D {size.x, size.y, 1};
	auto usage = vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::storage |
		vk::ImageUsageBits::transferSrc | // TODO
		vk::ImageUsageBits::transferDst;
	auto velInfo = vpp::ViewableImageCreateInfo::color(dev, extent,
		usage, {vk::Format::r16g16b16a16Sfloat}).value();
	velocity_ = {dev, velInfo};
	velocity0_ = {dev, velInfo};

	// constexpr auto csz = vk::ComponentSwizzle::zero;
	constexpr auto csr = vk::ComponentSwizzle::r;
	// constexpr auto csg = vk::ComponentSwizzle::g;
	// constexpr auto csb = vk::ComponentSwizzle::b;
	// constexpr auto csa = vk::ComponentSwizzle::a;
	constexpr auto cso = vk::ComponentSwizzle::one;
	// constexpr auto csi = vk::ComponentSwizzle::identity;

	auto scalarInfo = vpp::ViewableImageCreateInfo::color(dev, extent,
		usage, {vk::Format::r32Sfloat}).value();
	scalarInfo.view.components = {csr, csr, csr, cso};
	density_ = {dev, scalarInfo};
	density0_ = {dev, scalarInfo};

	pressure_ = {dev, scalarInfo};
	pressure0_ = {dev, scalarInfo};

	divergence_ = {dev, scalarInfo};

	auto fam = dev.queueSubmitter().queue().family();
	auto cmdBuf = dev.commandAllocator().get(fam);
	auto layout = vk::ImageLayout::undefined;

	vk::beginCommandBuffer(cmdBuf, {});
	auto images = {&density_, &density0_, &velocity_, &velocity0_,
		&pressure_, &pressure0_, &divergence_};
	for(auto img : images) {
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
	constexpr auto uboSize = sizeof(float) * 8;
	ubo_ = {dev.bufferAllocator(), uboSize,
		vk::BufferUsageBits::uniformBuffer, 4u, dev.hostMemoryTypes()};

	advectDensityDs_ = {dev.descriptorAllocator(), dsLayout_};
	advectVelocityDs_ = {dev.descriptorAllocator(), dsLayout_};
	divergenceDs_ = {dev.descriptorAllocator(), dsLayout_};
	pressureDs0_ = {dev.descriptorAllocator(), dsLayout_};
	pressureDs1_ = {dev.descriptorAllocator(), dsLayout_};
	projectDs_ = {dev.descriptorAllocator(), dsLayout_};

	using VI = const vpp::ViewableImage;
	auto updateDs = [&](auto& ds, VI* a, VI* b, VI* c, VI* d) {
		constexpr auto layout = vk::ImageLayout::general;
		vpp::DescriptorSetUpdate update(ds);

		for(auto& l : {a, b, c}) {
			if(l) {
				update.storage({{{}, l->vkImageView(), layout}});
			} else {
				update.skip();
			}
		}

		if(d) {
			update.imageSampler({{{}, d->vkImageView(), layout}});
		} else {
			update.skip();
		}

		update.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
	};

	// advect velocity: vel0, vel0 -> vel
	// divergence: vel -> div
	// project: div, vel -> vel0
	// advect density: dens0, vel0 -> dens

	updateDs(advectVelocityDs_, &velocity_, nullptr, &velocity0_, &velocity0_);
	updateDs(divergenceDs_, &divergence_, &velocity_, nullptr, nullptr);
	updateDs(pressureDs0_, &pressure0_, &pressure_, &divergence_, nullptr);
	updateDs(pressureDs1_, &pressure_, &pressure0_, &divergence_, nullptr);
	updateDs(projectDs_, &velocity0_, &velocity_, &pressure_, nullptr);

	updateDs(advectDensityDs_, &density_, nullptr, &velocity0_, &density0_);
}

void FluidSystem::updateDevice(float dt, nytl::Vec2f mp0, nytl::Vec2f mp1) {
	auto map = ubo_.memoryMap();
	auto data = map.span();
	write(data, mp0);
	write(data, mp1);
	write(data, dt);
	write(data, velocityFac);
	write(data, densityFac);
	write(data, radius);
}

void FluidSystem::compute(vk::CommandBuffer cb) {
	// dispatch sizes
	auto dx = size_.x / 16;
	auto dy = size_.y / 16;

	// sync util
	// we actually need a lot of synchronization here since we swap reading/
	// writing from/to images all the time and have to make sure the previous
	// command finished
	// NOTE: the sync in this proc is probably not fully correct...
	constexpr vk::ImageLayout general = vk::ImageLayout::general;
	using PSB = vk::PipelineStageBits;
	using AB = vk::AccessBits;
	auto readWrite = AB::shaderRead | AB::shaderWrite;
	auto barrier = [&](auto& img, auto srca, auto dsta) {
		vk::ImageMemoryBarrier barrier;
		barrier.oldLayout = general;
		barrier.newLayout = general;
		barrier.image = img.vkImage();
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		barrier.srcAccessMask = srca;
		barrier.dstAccessMask = dsta;
		return barrier;
	};

	auto insertBarrier = [&](auto srcs, auto dsts,
			std::initializer_list<vk::ImageMemoryBarrier> barriers) {
		vk::cmdPipelineBarrier(cb, srcs, dsts, {}, {}, {}, barriers);
		// nytl::unused(srcs, dsts, barriers);
	};

	// make sure reading the images is finished
	insertBarrier(PSB::fragmentShader, PSB::computeShader,  {
		barrier(velocity_, AB::shaderRead, AB::shaderWrite),
		barrier(density_, AB::shaderRead, AB::shaderWrite),
		barrier(divergence_, AB::shaderRead, AB::shaderWrite),
		barrier(pressure_, AB::shaderRead, AB::shaderWrite),
	});

	// == velocity ==
	// advect velocity
	vk::ImageCopy full;
	full.dstSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
	full.srcSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
	full.extent = {size_.x, size_.y, 1u};

	vk::cmdCopyImage(cb, velocity0_.image(), vk::ImageLayout::general,
		velocity_.image(), vk::ImageLayout::general, {full});

	insertBarrier(PSB::transfer, PSB::computeShader, {
		barrier(velocity_, AB::transferWrite, readWrite),
	});

	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, advectVel_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {advectVelocityDs_}, {});
	vk::cmdDispatch(cb, dx, dy, 1);

	insertBarrier(PSB::computeShader, PSB::computeShader, {
		barrier(velocity_, readWrite, readWrite),
	});

	// compute divergence
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, divergencePipe_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {divergenceDs_}, {});
	vk::cmdDispatch(cb, dx, dy, 1);

	// iterate pressure
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pressureIteration_);
	for(auto i = 0u; i < pressureIterations / 2; ++i) {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			pipeLayout_, 0, {pressureDs0_}, {});
		vk::cmdDispatch(cb, dx, dy, 1);

		insertBarrier(PSB::computeShader, PSB::computeShader, {
			barrier(pressure_, readWrite, readWrite),
		});

		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			pipeLayout_, 0, {pressureDs1_}, {});
		vk::cmdDispatch(cb, dx, dy, 1);

		insertBarrier(PSB::computeShader, PSB::computeShader, {
			barrier(pressure0_, readWrite, readWrite),
		});
	}

	// project velocity
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, project_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {projectDs_}, {});
	vk::cmdDispatch(cb, dx, dy, 1);

	insertBarrier(PSB::computeShader, PSB::computeShader, {
		barrier(velocity0_, readWrite, readWrite),
	});

	// == density ==
	vk::cmdCopyImage(cb, density_.image(), vk::ImageLayout::general,
		density0_.image(), vk::ImageLayout::general, {full});

	insertBarrier(PSB::transfer, PSB::computeShader, {
		barrier(density0_, AB::transferWrite, AB::shaderRead),
	});

	// advect
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, advectDens_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {advectDensityDs_}, {});
	vk::cmdDispatch(cb, dx, dy, 1);

	// make sure writing has finished before reading
	insertBarrier(PSB::computeShader, PSB::fragmentShader, {
		barrier(density_, AB::shaderWrite, AB::shaderRead),
		barrier(velocity_, AB::shaderWrite, AB::shaderRead),
		barrier(pressure_, AB::shaderWrite, AB::shaderRead),
		barrier(divergence_, AB::shaderWrite, AB::shaderRead),
	});
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
			changeView_ = system_->density().vkImageView();
		} else if(ev.keycode == ny::Keycode::v) {
			changeView_ = system_->velocity().vkImageView();
		} else if(ev.keycode == ny::Keycode::q) {
			changeView_ = system_->divergence().vkImageView();
		} else if(ev.keycode == ny::Keycode::p) {
			changeView_ = system_->pressure().vkImageView();
		}
	}

	void update(double dt) override {
		App::update(dt);
		dt_ = dt;
	}

	void updateDevice() override {
		App::updateDevice();
		system_->updateDevice(dt_, prevPos_, mpos_);
		prevPos_ = mpos_;

		if(changeView_) {
			vpp::DescriptorSetUpdate update(ds_);
			update.imageSampler({{{}, changeView_, vk::ImageLayout::general}});
			changeView_ = {};
			rerecord();
		}
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		using namespace nytl::vec::cw::operators;
		mpos_ = 512.f * (nytl::Vec2f(ev.position) / window().size());
	}

	void mouseButton(const ny::MouseButtonEvent& ev) override {
		App::mouseButton(ev);
		if(ev.button == ny::MouseButton::left) {
			system_->densityFac = ev.pressed * 0.5;
		} else if(ev.button == ny::MouseButton::right) {
			system_->velocityFac = ev.pressed * 0.5;
		}
	}

	void mouseWheel(const ny::MouseWheelEvent& ev) override {
		App::mouseWheel(ev);
		system_->radius *= std::pow(1.1, ev.value.y);
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
	nytl::Vec2f mpos_ {};
	nytl::Vec2f prevPos_ {};

	vk::ImageView changeView_ {};
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
