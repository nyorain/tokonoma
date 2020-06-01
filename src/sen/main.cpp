#include <tkn/singlePassApp.hpp>
#include <tkn/bits.hpp>
#include <tkn/window.hpp>
#include <tkn/transform.hpp>
#include <tkn/ccam.hpp>
#include <tkn/features.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tkn/stb_image_write.h>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/formats.hpp>
#include <vpp/image.hpp>
#include <vpp/imageOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/vk.hpp>
#include <nytl/mat.hpp>

#include <shaders/tkn.fullscreen.vert.h>
#include <shaders/sen.sen.frag.h>
#include <shaders/sen.senpt.frag.h>
#include <shaders/sen.senr.vert.h>
#include <shaders/sen.senr.frag.h>
#include <shaders/sen.sen.comp.h>

#include "render.hpp"

// TODO: could be optimized by using smaller images for small faces
constexpr auto faceWidth = 1024u;
constexpr auto faceHeight = 1024u;

class SenApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		initBoxes();
		initRaytracePipe();
		initPathtracePipe();
		initRasterPipe();
		initComputePipe();

		// XXX
		// VkPhysicalDeviceSubgroupProperties sp {};
		// sp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
		// sp.pNext = nullptr;
//
		// VkPhysicalDeviceProperties2 props {};
		// props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		// props.pNext = &sp;
//
		// auto phdev = (VkPhysicalDevice) vulkanDevice().vkPhysicalDevice();
		// vkGetPhysicalDeviceProperties2(phdev, &props);
//
		// dlg_info("shuffle: {}", (sp.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0);
		// dlg_info("size: {}", sp.subgroupSize);
		// dlg_info("compute: {}", sp.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT);

		return true;
	}

	bool features(tkn::Features& enable, const tkn::Features& supported) override {
		if(!App::features(enable, supported)) {
			return false;
		}

		// TODO: dont require the feature
		if(!supported.base.features.fragmentStoresAndAtomics) {
			return false;
		}

		enable.base.features.fragmentStoresAndAtomics = true;
		return true;
	}

	void initComputePipe() {
		auto& dev = vkDevice();
		auto dsBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute)
		};

		comp_.dsLayout = {dev, dsBindings};
		comp_.pipeLayout = {dev, {{comp_.dsLayout.vkHandle()}}, {}};

		vpp::ShaderModule compShader(dev, sen_sen_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = comp_.pipeLayout;
		cpi.stage.module = compShader;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;

		comp_.pipe = {dev, cpi};

		// ds
		comp_.ds = {dev.descriptorAllocator(), comp_.dsLayout};
		vpp::DescriptorSetUpdate dsu(comp_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{boxesBuf_}}});
		dsu.storage({{{}, lightTex_.vkImageView(), vk::ImageLayout::general}});
		dsu.apply();
	}

	void initPathtracePipe() {
		auto& dev = vkDevice();

		auto dsBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::fragment)
		};

		pt_.dsLayout = {dev, dsBindings};
		pt_.pipeLayout = {dev, {{pt_.dsLayout.vkHandle()}}, {}};

		vpp::ShaderModule fullscreenShader(dev, tkn_fullscreen_vert_data);
		vpp::ShaderModule textureShader(dev, sen_senpt_frag_data);
		vpp::GraphicsPipelineInfo pipeInfo(renderPass(), pt_.pipeLayout, {{{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}}}, 0, samples());

		pt_.pipe = {dev, pipeInfo.info()};

		// box images
		std::uint32_t width = 6 * faceWidth;
		std::uint32_t height = boxes_.size() * faceHeight;
		// auto format = vk::Format::r32g32b32a32Sfloat;
		auto format = vk::Format::r32Uint;
		auto usage = vk::ImageUsageBits::transferDst |
			vk::ImageUsageBits::transferSrc |
			vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled;
		atlasSize_ = {width, height};

		auto imgi = vpp::ViewableImageCreateInfo(format,
			vk::ImageAspectBits::color, {width, height}, usage);
		dlg_assert(vpp::supported(vkDevice(), imgi.img));

		// TODO: we could create second view with r8g8b8a8 format
		// and then use linear interpolation in sampler,
		// use mutable format for that
		// imgi.img.flags = vk::ImageCreateBits::mutableFormat;

		lightTex_ = {dev.devMemAllocator(), imgi};

		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		vk::ImageMemoryBarrier barrier;
		barrier.image = lightTex_.image();
		barrier.oldLayout = vk::ImageLayout::undefined;
		barrier.srcAccessMask = {};
		barrier.newLayout = vk::ImageLayout::general;
		barrier.dstAccessMask = vk::AccessBits::memoryWrite; // TODO: not sure what clear image is
		barrier.subresourceRange = {vk::ImageAspectBits::color, 0, 1, 0, 1};

		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::topOfPipe,
			vk::PipelineStageBits::transfer, {}, {}, {}, {{barrier}});

		auto clearValue = vk::ClearColorValue{};
		clearValue.uint32 = {0x44444444u, 0u, 0u, 0u};
		auto range = vk::ImageSubresourceRange {};
		range.aspectMask = vk::ImageAspectBits::color;
		range.baseArrayLayer = 0;
		range.layerCount = 1u;
		range.levelCount = 1;
		vk::cmdClearColorImage(cb, lightTex_.image(),
			vk::ImageLayout::general, clearValue, {{range}});

		vk::endCommandBuffer(cb);

		vk::SubmitInfo si {};
		si.commandBufferCount = 1u;
		si.pCommandBuffers = &cb.vkHandle();
		qs.wait(qs.add(si));

		// ds
		pt_.ds = {dev.descriptorAllocator(), pt_.dsLayout};
		vpp::DescriptorSetUpdate dsu(pt_.ds);
		dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		dsu.storage({{boxesBuf_.buffer(), boxesBuf_.offset(), boxesBuf_.size()}});
		dsu.storage({{{}, lightTex_.vkImageView(), vk::ImageLayout::general}});
		dsu.apply();
	}

	void initRaytracePipe() {
		auto& dev = vkDevice();
		auto mem = dev.hostMemoryTypes();

		// TODO: use real size instead of larger dummy
		ubo_ = {dev.bufferAllocator(), sizeof(float) * 128,
			vk::BufferUsageBits::uniformBuffer, mem};

		auto renderBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
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
		vpp::ShaderModule textureShader(dev, sen_sen_frag_data);
		vpp::GraphicsPipelineInfo pipeInfo(renderPass(), pipeLayout_, {{{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}}}, 0, samples());

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},  1, pipeInfo.info(),
			nullptr, vkpipe);
		pipe_ = {dev, vkpipe};

		// boxes
		// auto sizePerBox = 2 * sizeof(nytl::Mat4f) + sizeof(nytl::Vec4f);
		auto sizePerBox = sizeof(nytl::Mat4f) + sizeof(nytl::Vec4f);
		auto size = boxes_.size() * sizePerBox;
		boxesBuf_ = {dev.bufferAllocator(), size,
			vk::BufferUsageBits::storageBuffer, dev.hostMemoryTypes()};

		auto map = boxesBuf_.memoryMap();
		auto span = map.span();
		for(auto& b : boxes_) {
			tkn::write(span, b.box.inv);
			tkn::write(span, b.color);

			auto off = span.begin() - map.span().begin();
			b.bufOff = off;
		}

		// ds
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		dsu.storage({{boxesBuf_.buffer(), boxesBuf_.offset(), boxesBuf_.size()}});
		dsu.apply();
	}

	void initRasterPipe() {
		auto& dev = vkDevice();

		vk::SamplerCreateInfo sci {};
		sci.magFilter = vk::Filter::nearest;
		sci.minFilter = vk::Filter::nearest;
		sci.minLod = 0.0;
		sci.maxLod = 0.25;
		sci.mipmapMode = vk::SamplerMipmapMode::nearest;
		sampler_ = {dev, sci};

		// ds layout
		auto sceneBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment | vk::ShaderStageBits::vertex),
		};

		auto objectBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment, -1, 1, &sampler_.vkHandle()),
		};

		rasterSceneDsLayout_ = {dev, sceneBindings};
		rasterObjectDsLayout_ = {dev, objectBindings};
		rasterPipeLayout_ = {dev, {{
			rasterSceneDsLayout_.vkHandle(),
			rasterObjectDsLayout_.vkHandle()}}, {}};

		// pipe
		vpp::ShaderModule vertShader(dev, sen_senr_vert_data);
		vpp::ShaderModule fragShader(dev, sen_senr_frag_data);
		vpp::GraphicsPipelineInfo gpi(renderPass(), rasterPipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment}
		}}}, 0, samples());

		constexpr auto stride = 2 * sizeof(nytl::Vec3f);
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex
		};

		vk::VertexInputAttributeDescription attributes[2];
		attributes[0].format = vk::Format::r32g32b32Sfloat; // pos

		attributes[1].format = vk::Format::r32g32b32Sfloat; // normal
		attributes[1].offset = sizeof(float) * 3; // pos
		attributes[1].location = 1;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 2u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {}, 1, gpi.info(), NULL, vkpipe);
		rasterPipe_ = {dev, vkpipe};

		// buffer: box
		auto boxModelSize = 36u * sizeof(std::uint32_t) // indices
			+ 24 * sizeof(nytl::Vec3f) // points
			+ 24 * sizeof(nytl::Vec3f); // normal
		boxModel_ = {dev.bufferAllocator(), boxModelSize,
			vk::BufferUsageBits::indexBuffer |
				vk::BufferUsageBits::vertexBuffer |
				vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};

		// fill
		auto stage = vpp::SubBuffer{dev.bufferAllocator(), boxModelSize,
			vk::BufferUsageBits::transferSrc, dev.hostMemoryTypes()};
		auto map = stage.memoryMap();
		auto span = map.span();

		// write indices
		for(auto i = 0u; i < 24; i += 4) {
			tkn::write<std::uint32_t>(span, i + 0);
			tkn::write<std::uint32_t>(span, i + 1);
			tkn::write<std::uint32_t>(span, i + 2);

			tkn::write<std::uint32_t>(span, i + 0);
			tkn::write<std::uint32_t>(span, i + 2);
			tkn::write<std::uint32_t>(span, i + 3);
		}

		// write positions and normals
		for(auto i = 0u; i < 3u; ++i) {
			nytl::Vec3f n {
				float(i == 0),
				float(i == 1),
				float(i == 2)
			};

			nytl::Vec3f x = nytl::Vec3f{
				float(i == 1),
				float(i == 2),
				float(i == 0)
			};

			nytl::Vec3f y = nytl::Vec3f{
				float(i == 2),
				float(i == 0),
				float(i == 1)
			};

			tkn::write(span, n - x + y); tkn::write(span, n);
			tkn::write(span, n + x + y); tkn::write(span, n);
			tkn::write(span, n + x - y); tkn::write(span, n);
			tkn::write(span, n - x - y); tkn::write(span, n);

			// other (mirrored) side
			n *= -1.f;
			tkn::write(span, n - x - y); tkn::write(span, n);
			tkn::write(span, n + x - y); tkn::write(span, n);
			tkn::write(span, n + x + y); tkn::write(span, n);
			tkn::write(span, n - x + y); tkn::write(span, n);
		}

		// upload
		auto& qs = dev.queueSubmitter();
		auto cb = qs.device().commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});
		vk::BufferCopy region;
		region.dstOffset = boxModel_.offset();
		region.srcOffset = stage.offset();
		region.size = boxModelSize;
		vk::cmdCopyBuffer(cb, stage.buffer(), boxModel_.buffer(), {{region}});
		vk::endCommandBuffer(cb);

		// execute
		// TODO: could be batched with other work; we wait here
		vk::SubmitInfo submission;
		submission.commandBufferCount = 1;
		submission.pCommandBuffers = &cb.vkHandle();
		qs.wait(qs.add(submission));

		// ubo
		auto sceneUboSize = sizeof(nytl::Mat4f) // proj matrix
			+ 2 * sizeof(nytl::Vec2f) // face & atlas size in pixels
			+ sizeof(nytl::Vec3f) // viewPos
			+ sizeof(float); // show light tex
		rasterSceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// scene ds
		rasterSceneDs_ = {dev.descriptorAllocator(), rasterSceneDsLayout_};
		vpp::DescriptorSetUpdate sdsu(rasterSceneDs_);
		sdsu.uniform({{rasterSceneUbo_.buffer(),
			rasterSceneUbo_.offset(), rasterSceneUbo_.size()}});
		sdsu.apply();

		// boxes
		auto bufSize = 2 * sizeof(nytl::Mat4f) + sizeof(nytl::Vec4f)
			+ sizeof(std::uint32_t);
		auto i = 0u;
		for(auto& b: boxes_) {
			b.rasterdata = {dev.bufferAllocator(), bufSize,
				vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
			{
				auto map = b.rasterdata.memoryMap();
				auto span = map.span();
				tkn::write(span, b.box.transform);
				auto nm = nytl::Mat4f(transpose(b.box.inv));
				tkn::write(span, nm);
				tkn::write(span, b.color);
				tkn::write(span, i);
			}

			b.ds = {dev.descriptorAllocator(), rasterObjectDsLayout_};
			vpp::DescriptorSetUpdate update(b.ds);
			update.uniform({{b.rasterdata.buffer(),
				b.rasterdata.offset(), b.rasterdata.size()}});
			update.imageSampler({{{}, lightTex_.vkImageView(),
				vk::ImageLayout::general}});
			++i;
		}
	}

	void initBoxes() {
		boxes_.emplace_back(); // rotating inside
		boxes_.back().box = Box {{-2.f, -3.f, 0.f},
			{1.f, 0.0f, 0.0f},
			{0.f, 1.f, 0.f},
			{0.0f, 0.f, 1.f},
		};
		boxes_.back().color = {1.0f, 0.8f, 0.2f};

		boxes_.emplace_back(); // back
		boxes_.back().box = Box {{0.f, 0.f, -4.f},
			{4.f, 0.f, 0.f},
			{0.f, 4.f, 0.f},
			{0.f, 0.f, 0.1f},
		};
		boxes_.back().color = {1.f, 1.f, 1.f};

		boxes_.emplace_back(); // left
		boxes_.back().box = Box {{-4.f, 0.f, 0.f},
			{0.1f, 0.f, 0.f},
			{0.f, 4.f, 0.f},
			{0.f, 0.f, 4.f},
		};
		boxes_.back().color = {1.f, 0.2f, 0.2f};

		boxes_.emplace_back(); // right
		boxes_.back().box = Box {{4.f, 0.f, 0.f},
			{0.1f, 0.f, 0.f},
			{0.f, 4.f, 0.f},
			{0.f, 0.f, 4.f},
		};
		boxes_.back().color = {0.2f, 1.f, 0.2f};

		boxes_.emplace_back(); // bot
		boxes_.back().box = Box {{0.f, -4.1f, 0.f},
			{4.1f, 0.f, 0.f},
			{0.f, 0.1f, 0.f},
			{0.f, 0.f, 4.0f},
		};
		boxes_.back().color = {1.f, 1.f, 1.f};

		boxes_.emplace_back(); // top
		boxes_.back().box = Box {{0.f, 4.1f, 0.f},
			{4.1f, 0.f, 0.f},
			{0.f, 0.1f, 0.f},
			{0.f, 0.f, 4.0f},
		};
		boxes_.back().color = {1.f, 1.f, 1.f};

		boxes_.emplace_back(); // inside
		boxes_.back().box = Box {{2.f, -3.f, 0.f},
			{1.f, 0.0f, 0.5f},
			{0.f, 1.f, 0.f},
			{-0.5f, 0.f, 1.f},
		};
		boxes_.back().color = {0.2f, 0.2f, 1.f};
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		App::mouseMove(ev);
		camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	void beforeRender(vk::CommandBuffer cb) override {
		if(renderMode_ == 3) {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute,
				comp_.pipe);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
				comp_.pipeLayout, 0, {{comp_.ds.vkHandle()}}, {});
			vk::cmdDispatch(cb, 512, 512, 1);
		}
	}

	void render(vk::CommandBuffer cb) override {
		if(renderMode_ == 0 || renderMode_ == 3) {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, rasterPipe_);
			vk::cmdBindIndexBuffer(cb, boxModel_.buffer(),
				boxModel_.offset(), vk::IndexType::uint32);
			vk::cmdBindVertexBuffers(cb, 0, 1, {boxModel_.buffer()},
				{boxModel_.offset() + 36 * sizeof(std::uint32_t)});
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				rasterPipeLayout_, 0, {{rasterSceneDs_.vkHandle()}}, {});

			for(auto& b : boxes_) {
				vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
					rasterPipeLayout_, 1, {{b.ds.vkHandle()}}, {});
				vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
			}
		} else if(renderMode_ == 1) {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				pipeLayout_, 0, {{ds_.vkHandle()}}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0);
		} else if(renderMode_ == 2) {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pt_.pipe);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				pt_.pipeLayout, 0, {{pt_.ds.vkHandle()}}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0);
		}
	}

	bool key(const swa_key_event& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == swa_key_k0) {
			renderMode_ = 0;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == swa_key_k1) {
			renderMode_ = 1;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == swa_key_k2) {
			renderMode_ = 2;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == swa_key_k3) {
			renderMode_ = 3;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == swa_key_r) {
			renderMode_ = (renderMode_ + 1) % 4;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == swa_key_l) {
			camera_.needsUpdate = true; // updates ubo
			if(ev.modifiers & swa_keyboard_mod_shift) {
				showLightTex_ = (showLightTex_ + 2) % 3;
			} else {
				showLightTex_ = (showLightTex_ + 1) % 3;
			}
			return true;
		} else if(ev.keycode == swa_key_i) {
			// save image
			saveImage_ = true;
		}

		return false;
	}

	void update(double dt) override {
		App::update(dt);
		time_ += dt;
		camera_.update(swaDisplay(), dt);
		App::scheduleRedraw();
	}

	void updateDevice() override {
		// raytracing
		auto rmap = ubo_.memoryMap();
		auto rspan = rmap.span();
		auto aspect = float(windowSize().x) / windowSize().y;

		tkn::write(rspan, camera_.position());
		tkn::write(rspan, 0.f); // padding
		tkn::write(rspan, camera_.dir());
		tkn::write(rspan, 0.f); // padding
		tkn::write(rspan, *camera_.perspectiveFov());
		tkn::write(rspan, aspect);
		tkn::write(rspan, float(windowSize().x));
		tkn::write(rspan, float(windowSize().y));
		tkn::write(rspan, nytl::Vec2f{faceWidth, faceHeight});
		tkn::write(rspan, float(time_));

		// update scene ubo
		if(camera_.needsUpdate) {
			camera_.needsUpdate = false;

			// raster
			{
				auto map = rasterSceneUbo_.memoryMap();
				auto span = map.span();
				tkn::write(span, camera_.viewProjectionMatrix());
				tkn::write(span, camera_.position());
				tkn::write(span, showLightTex_);
				tkn::write(span, nytl::Vec2f{faceWidth, faceHeight});
				tkn::write(span, nytl::Vec2f(atlasSize_));
			}
		}

		// update rotating box
		auto& b = boxes_[0];
		float a = 0.5 * time_;
		auto rot = nytl::Mat3f(tkn::rotateMat(nytl::Vec3f{0.f, 1.f, 0.f}, a));
		b.box = {{-2.f, -3.f, 0.f}, rot};

		{
			auto map = boxesBuf_.memoryMap();
			auto span = map.span();
			tkn::write(span, b.box.inv);
		}

		{
			auto map = b.rasterdata.memoryMap();
			auto span = map.span();
			tkn::write(span, b.box.transform);
			auto nm = nytl::Mat4f(transpose(b.box.inv));
			tkn::write(span, nm);
		}

		// save texture
		if(saveImage_) {
			dlg_error("TODO: image saving not updated/reimplemented yet");
			/*
			auto work = vpp::retrieveStaging(lightTex_.image(),
				vk::Format::r32Uint, vk::ImageLayout::general,
				{atlasSize_.x, atlasSize_.y, 1},
				{vk::ImageAspectBits::color, 0, 0});
			auto d = work.data();
			std::vector<std::byte> buf(d.size());
			std::memcpy(buf.data(), work.data().data(), buf.size());

			auto name = "lightTex.bmp";
			stbi_write_png(name, atlasSize_.x, atlasSize_.y, 4,
				buf.data(), atlasSize_.x * 4);
			saveImage_ = false;
			*/
		}
	}

	void resize(unsigned w, unsigned h) override {
		App::resize(w, h);
		camera_.aspect({w, h});
	}

	bool needsDepth() const override {
		return true;
	}

	const char* name() const override { return "sen"; }

private:
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::Pipeline pipe_;

	vpp::TrDs ds_;
	vpp::SubBuffer ubo_;
	vpp::SubBuffer boxesBuf_;

	vpp::ViewableImage lightTex_; // glboal light effects; atlas

	// raster
	vpp::Pipeline rasterPipe_;
	vpp::PipelineLayout rasterPipeLayout_;
	vpp::TrDsLayout rasterSceneDsLayout_;
	vpp::TrDsLayout rasterObjectDsLayout_;
	vpp::TrDs rasterSceneDs_;
	vpp::SubBuffer boxModel_;
	vpp::SubBuffer rasterSceneUbo_;
	vpp::Sampler sampler_;

	// compute
	struct {
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
	} comp_;

	// path trace
	struct {
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;

		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
	} pt_;

	std::vector<RenderBox> boxes_;

	tkn::ControlledCamera camera_;
	float time_ {};
	std::uint32_t showLightTex_ {0};

	int renderMode_ {3};
	nytl::Vec2ui atlasSize_;
	bool saveImage_ {false};
};

int main(int argc, const char** argv) {
	return tkn::appMain<SenApp>(argc, argv);
}
