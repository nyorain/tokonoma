// TODO/current state:
// - pressure correction seems not to work for larger dt (it at least
//   seems like that is the problem. Maybe we can't just move dt out
//   of the linear system?)
//  	- also for smaller dt: density can just vanish which should
//  	  not happen (mass preservation). only approximation errors?
// - diffusion currently completely disabled (diffuse.comp broken and
//   disabled needs correct boundary conditions. But probably not
//   really worth it anyways?)
// - not 100% sure about the other boundary conditions

#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/bits.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/formats.hpp>
#include <vpp/image.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/submit.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <ny/key.hpp>
#include <ny/mouseButton.hpp>
#include <ny/appContext.hpp>
#include <ny/keyboardContext.hpp>
#include <dlg/dlg.hpp>
#include <cstring>
#include <random>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/fluids.fluid_texture.frag.h>
#include <shaders/fluids.advect.vel.comp.h>
#include <shaders/fluids.advect.dens.comp.h>
#include <shaders/fluids.divergence.comp.h>
#include <shaders/fluids.pressure.comp.h>
#include <shaders/fluids.project.comp.h>
#include <shaders/fluids.diffuse.dens.comp.h>

// == FluidSystem ==
class FluidSystem {
public:
	static constexpr auto pressureIterations = 10u;
	static constexpr auto diffuseDensIterations = 0u;

	float velocityFac {0.0};
	float densityFac {0.0};
	float radius {20.f};

public:
	FluidSystem(vpp::Device& dev, nytl::Vec2ui size);

	void updateDevice(float dt, nytl::Vec2f mp0, nytl::Vec2f mp1);
	void compute(vk::CommandBuffer);

	const auto& density() const { return density_; }
	const auto& velocity() const { return velocity_; }
	const auto& pressure() const { return pressure_; }
	const auto& divergence() const { return divergence_; }
	auto size() const { return size_; }

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

	vpp::Pipeline diffuseDens_;
	vpp::TrDs diffuseDens0_;
	vpp::TrDs diffuseDens1_;

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
	info.maxLod = 0;
	info.mipmapMode = vk::SamplerMipmapMode::nearest;
	// velocity border condition: 0
	info.addressModeU = vk::SamplerAddressMode::clampToBorder;
	info.addressModeV = vk::SamplerAddressMode::clampToBorder;
	info.borderColor = vk::BorderColor::floatTransparentBlack;
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
	pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};

	auto advectVelShader = vpp::ShaderModule(dev, fluids_advect_vel_comp_data);
	auto advectDensShader = vpp::ShaderModule(dev, fluids_advect_dens_comp_data);
	auto divergenceShader = vpp::ShaderModule(dev, fluids_divergence_comp_data);
	auto pressureShader = vpp::ShaderModule(dev, fluids_pressure_comp_data);
	auto projectShader = vpp::ShaderModule(dev, fluids_project_comp_data);
	auto diffuseDensShader = vpp::ShaderModule(dev, fluids_diffuse_dens_comp_data);

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

	auto diffuseDensInfo = advectInfoVel;
	diffuseDensInfo.stage.module = diffuseDensShader;

	auto pipes = vk::createComputePipelines(dev, {}, {{
		advectInfoVel,
		advectInfoDens,
		divergenceInfo,
		pressureInfo,
		projectInfo,
		diffuseDensInfo}});
	advectVel_ = {dev, pipes[0]};
	advectDens_ = {dev, pipes[1]};
	divergencePipe_ = {dev, pipes[2]};
	pressureIteration_ = {dev, pipes[3]};
	project_ = {dev, pipes[4]};
	diffuseDens_ = {dev, pipes[5]};

	// images
	auto usage = vk::ImageUsageBits::sampled |
		vk::ImageUsageBits::storage |
		vk::ImageUsageBits::transferSrc | // TODO
		vk::ImageUsageBits::transferDst;
	auto velInfo = vpp::ViewableImageCreateInfo(vk::Format::r16g16b16a16Sfloat,
		vk::ImageAspectBits::color, {size.x, size.y}, usage);
	dlg_assert(vpp::supported(dev, velInfo.img));
	velocity_ = {dev.devMemAllocator(), velInfo};
	velocity0_ = {dev.devMemAllocator(), velInfo};

	// constexpr auto csz = vk::ComponentSwizzle::zero;
	constexpr auto csr = vk::ComponentSwizzle::r;
	// constexpr auto csg = vk::ComponentSwizzle::g;
	// constexpr auto csb = vk::ComponentSwizzle::b;
	// constexpr auto csa = vk::ComponentSwizzle::a;
	constexpr auto cso = vk::ComponentSwizzle::one;
	// constexpr auto csi = vk::ComponentSwizzle::identity;

	auto scalarInfo = vpp::ViewableImageCreateInfo(vk::Format::r32Sfloat,
		vk::ImageAspectBits::color, {size.x, size.y}, usage);
	scalarInfo.view.components = {csr, csr, csr, cso};
	dlg_assert(vpp::supported(dev, velInfo.img));
	density_ = {dev.devMemAllocator(), scalarInfo};
	density0_ = {dev.devMemAllocator(), scalarInfo};

	pressure_ = {dev.devMemAllocator(), scalarInfo};
	pressure0_ = {dev.devMemAllocator(), scalarInfo};

	divergence_ = {dev.devMemAllocator(), scalarInfo};

	auto fam = dev.queueSubmitter().queue().family();
	auto cmdBuf = dev.commandAllocator().get(fam);
	auto layout = vk::ImageLayout::undefined;

	vk::beginCommandBuffer(cmdBuf, {});
	auto images = {&density_, &density0_, &velocity_, &velocity0_,
		&pressure_, &pressure0_, &divergence_};
	for(auto img : images) {
		vk::ImageMemoryBarrier barrier;
		barrier.image = img->image();
		barrier.oldLayout = layout;
		barrier.srcAccessMask = {};
		barrier.newLayout = vk::ImageLayout::general;
		barrier.dstAccessMask = vk::AccessBits::transferWrite;
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};
		vk::cmdPipelineBarrier(cmdBuf, vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

		auto color = vk::ClearColorValue {{0.f, 0.f, 0.f, 0.f}};
		vk::cmdClearColorImage(cmdBuf, img->vkImage(), vk::ImageLayout::general,
			color, {{{vk::ImageAspectBits::color, 0, 1, 0, 1}}});
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
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

	advectDensityDs_ = {dev.descriptorAllocator(), dsLayout_};
	advectVelocityDs_ = {dev.descriptorAllocator(), dsLayout_};
	divergenceDs_ = {dev.descriptorAllocator(), dsLayout_};
	pressureDs0_ = {dev.descriptorAllocator(), dsLayout_};
	pressureDs1_ = {dev.descriptorAllocator(), dsLayout_};
	projectDs_ = {dev.descriptorAllocator(), dsLayout_};
	diffuseDens0_ = {dev.descriptorAllocator(), dsLayout_};
	diffuseDens1_ = {dev.descriptorAllocator(), dsLayout_};

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

	// == naming and swapping convention ==
	// the first pass should always read from the not 0 texture
	// the last pass should always render to the not 0 texture
	// the 0 texture is seen as temporary storage

	// advect velocity: vel, vel -> vel0
	// divergence: vel0 -> div
	// project: div, vel0 -> vel

	// advect density: dens, vel -> dens0
	// diffuse density loop: dens0 -> dens; dens -> dens0
	// final diffuse density: dens0 -> dens

	// the 0,1 ds's should be named in the order in that they appear

	updateDs(advectVelocityDs_, &velocity0_, nullptr, &velocity_, &velocity_);
	updateDs(divergenceDs_, &divergence_, &velocity0_, nullptr, nullptr);
	updateDs(pressureDs0_, &pressure0_, &pressure_, &divergence_, nullptr);
	updateDs(pressureDs1_, &pressure_, &pressure0_, &divergence_, nullptr);
	updateDs(projectDs_, &velocity_, &velocity0_, &pressure_, nullptr);

	updateDs(advectDensityDs_, &density0_, nullptr, &velocity_, &density_);
	updateDs(diffuseDens0_, &density_, &density0_, nullptr, nullptr);
	updateDs(diffuseDens1_, &density0_, &density_, nullptr, nullptr);
}

void FluidSystem::updateDevice(float dt, nytl::Vec2f mp0, nytl::Vec2f mp1) {
	auto map = ubo_.memoryMap();
	auto data = map.span();
	tkn::write(data, mp0);
	tkn::write(data, mp1);
	tkn::write(data, dt);
	tkn::write(data, velocityFac);
	tkn::write(data, densityFac);
	tkn::write(data, radius);
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

	// when adding additional passes we might need one additional
	// copy image at the beginning/end to make sure the number of
	// swaps comes out even
	vk::ImageCopy full;
	full.dstSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
	full.srcSubresource = {vk::ImageAspectBits::color, 0, 0, 1};
	full.extent = {size_.x, size_.y, 1u};
	// vk::cmdCopyImage(cb, velocity0_.image(), vk::ImageLayout::general,
	// 	velocity_.image(), vk::ImageLayout::general, {full});
	// insertBarrier(PSB::transfer, PSB::computeShader, {
	// 	barrier(velocity_, AB::transferWrite, readWrite),
	// });

	// make sure reading the images is finished
	insertBarrier(PSB::fragmentShader, PSB::computeShader | PSB::transfer, {
		barrier(velocity_, AB::shaderRead, AB::shaderWrite),
		barrier(density_, AB::shaderRead, AB::shaderWrite),
		barrier(divergence_, AB::shaderRead, AB::shaderWrite),
		barrier(pressure_, AB::shaderRead, AB::shaderWrite | AB::transferWrite),
	});

	// we clear pressure since we want to use vec4(0) as initial guess
	// for pressure (to get better approximations)
	// might also work if we don't do this
	// NOTE: seems to works better if we don't do this, seems like the
	// pressure from the previous frame is a way better guess than just 0
	/*
	auto color = vk::ClearColorValue {{0.f, 0.f, 0.f, 0.f}};
	vk::cmdClearColorImage(cb, pressure_.vkImage(), vk::ImageLayout::general,
		color, {{{vk::ImageAspectBits::color, 0, 1, 0, 1}}});

	insertBarrier(PSB::transfer, PSB::computeShader, {
		barrier(pressure_, AB::transferWrite, readWrite),
	});
	*/

	// == velocity ==
	// advect velocity (also applies external forces after advection)
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, advectVel_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {{advectVelocityDs_.vkHandle()}}, {});
	vk::cmdDispatch(cb, dx, dy, 1);

	insertBarrier(PSB::computeShader, PSB::computeShader, {
		barrier(velocity0_, readWrite, readWrite),
	});

	// compute divergence
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, divergencePipe_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {{divergenceDs_.vkHandle()}}, {});
	vk::cmdDispatch(cb, dx, dy, 1);

	insertBarrier(PSB::computeShader, PSB::computeShader, {
		barrier(divergence_, AB::shaderWrite, AB::shaderRead),
	});

	// iterate pressure
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pressureIteration_);
	for(auto i = 0u; i < pressureIterations / 2; ++i) {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			pipeLayout_, 0, {{pressureDs0_.vkHandle()}}, {});
		vk::cmdDispatch(cb, dx, dy, 1);

		insertBarrier(PSB::computeShader, PSB::computeShader, {
			barrier(pressure0_, readWrite, readWrite),
		});

		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			pipeLayout_, 0, {{pressureDs1_.vkHandle()}}, {});
		vk::cmdDispatch(cb, dx, dy, 1);

		insertBarrier(PSB::computeShader, PSB::computeShader, {
			barrier(pressure_, readWrite, readWrite),
		});
	}

	// project velocity
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, project_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {{projectDs_.vkHandle()}}, {});
	vk::cmdDispatch(cb, dx, dy, 1);

	insertBarrier(PSB::computeShader, PSB::computeShader | PSB::fragmentShader, {
		barrier(velocity_, readWrite, readWrite),
	});

	// == density ==
	// TODO: should be at end otherwise density texture read is out of date
	// vk::cmdCopyImage(cb, density0_.image(), vk::ImageLayout::general,
	// 	density_.image(), vk::ImageLayout::general, {full});
	// insertBarrier(PSB::transfer, PSB::computeShader, {
	// 	barrier(density0_, AB::transferWrite, AB::shaderRead),
	// });

	// advect
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, advectDens_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {{advectDensityDs_.vkHandle()}}, {});
	vk::cmdDispatch(cb, dx, dy, 1);

	insertBarrier(PSB::computeShader, PSB::computeShader, {
		barrier(density0_, readWrite, readWrite),
	});

	// diffuse
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, diffuseDens_);
	/*
	for(auto i = 0u; i < diffuseDensIterations / 2; ++i) {
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			pipeLayout_, 0, {diffuseDens0_}, {});
		vk::cmdDispatch(cb, dx, dy, 1);

		insertBarrier(PSB::computeShader, PSB::computeShader, {
			barrier(density_, readWrite, readWrite),
		});

		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			pipeLayout_, 0, {diffuseDens1_}, {});
		vk::cmdDispatch(cb, dx, dy, 1);

		insertBarrier(PSB::computeShader, PSB::computeShader, {
			barrier(density0_, readWrite, readWrite),
		});
	}
	*/

	// one final iterations for even swap count (taking the initial
	// advection into account)
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pipeLayout_, 0, {{diffuseDens0_.vkHandle()}}, {});
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
class FluidApp : public tkn::App {
public:
	bool init(const nytl::Span<const char*> args) override {
		if(!App::init(args)) {
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
		vpp::ShaderModule textureShader(dev, fluids_fluid_texture_frag_data);

		vpp::GraphicsPipelineInfo pipeInfo(renderPass(), pipeLayout_, {{{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}}});

		pipe_ = {dev, pipeInfo.info()};

		// system
		// NOTE: MUST be multiple of 16 due to work group size
		system_.emplace(vulkanDevice(), nytl::Vec2ui {256, 256});

		// mouse ubo
		mouseUbo_ = {dev.bufferAllocator(), sizeof(nytl::Vec2f),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// ds
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		auto iv = system_->density().vkImageView();
		vpp::DescriptorSetUpdate update(ds_);
		update.imageSampler({{{}, iv, vk::ImageLayout::general}});
		update.uniform({{{mouseUbo_}}});

		return true;
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == ny::Keycode::d) {
			changeView_ = system_->density().vkImageView();
			viewType_ = 1;
		} else if(ev.keycode == ny::Keycode::f) {
			changeView_ = system_->density().vkImageView();
			viewType_ = 3;
		} else if(ev.keycode == ny::Keycode::v) {
			changeView_ = system_->velocity().vkImageView();
			viewType_ = 2;
		} else if(ev.keycode == ny::Keycode::q) {
			changeView_ = system_->divergence().vkImageView();
			viewType_ = 4;
		} else if(ev.keycode == ny::Keycode::p) {
			changeView_ = system_->pressure().vkImageView();
			viewType_ = 1;
		} else if(ev.keycode == ny::Keycode::l) {
			changeView_ = system_->velocity().vkImageView();
			viewType_ = 5;
		} else {
			return false;
		}

		return true;
	}

	void update(double dt) override {
		App::scheduleRedraw();
		App::update(dt);
		dt_ = dt;
	}

	void updateDevice() override {
		App::updateDevice();
		using namespace nytl::vec::cw::operators;
		system_->updateDevice(dt_, system_->size() * prevPos_,
			system_->size() * mpos_);
		prevPos_ = mpos_;

		auto map = mouseUbo_.memoryMap();
		auto span = map.span();
		tkn::write(span, mpos_);

		if(changeView_) {
			vpp::DescriptorSetUpdate update(ds_);
			update.imageSampler({{{}, changeView_, vk::ImageLayout::general}});
			changeView_ = {};
			App::scheduleRerecord();
		}
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		using namespace nytl::vec::cw::operators;
		mpos_ = nytl::Vec2f(ev.position) / window().size();
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		auto kc = appContext().keyboardContext();
		auto mod = kc->modifiers() & ny::KeyboardModifier::shift;
		if(ev.button == ny::MouseButton::left && !mod) {
			system_->densityFac = ev.pressed * 0.01;
			system_->velocityFac = 0.f;
		} else if(ev.button == ny::MouseButton::right || mod) {
			system_->velocityFac = ev.pressed * 10.f;
		} else {
			return false;
		}

		return true;
	}

	bool mouseWheel(const ny::MouseWheelEvent& ev) override {
		if(App::mouseWheel(ev)) {
			return true;
		}

		system_->radius *= std::pow(1.05, ev.value.y);
		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		system_->compute(cb);
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::fragment,
			0u, 4u, &viewType_);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {{ds_.vkHandle()}}, {});
		vk::cmdDraw(cb, 6, 1, 0, 0);
	}

	const char* name() const override { return "Fluids"; }

protected:
	std::optional<FluidSystem> system_;
	float dt_ {};
	nytl::Vec2f mpos_ {};
	nytl::Vec2f prevPos_ {};

	vk::ImageView changeView_ {};
	unsigned viewType_ {1};
	vpp::SubBuffer mouseUbo_;

	vpp::Sampler sampler_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

// main
int main(int argc, const char** argv) {
	FluidApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
