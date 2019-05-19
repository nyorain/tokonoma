#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/bits.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/camera.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stage/stb_image_write.h>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/formats.hpp>
#include <vpp/image.hpp>
#include <vpp/imageOps.hpp>

#include <nytl/mat.hpp>
#include <ny/mouseButton.hpp>
#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>

#include <shaders/fullscreen.vert.h>
#include <shaders/sen.frag.h>
#include <shaders/senpt.frag.h>
#include <shaders/senr.vert.h>
#include <shaders/senr.frag.h>
#include <shaders/sen.comp.h>

// NOTE: started this as full-featured raytracing app based on the ideas
// from sen. Didn't go anywhere though (when i tried to at least somewhat
// correctly model the brdf and noticed that the sen model is just
// really not good...).
// doesn't compile, should probably be deleted

class Lith : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		initGfx();
		initCompute();
		initGeometry();

		return true;
	}

	// initializes raterization pipeline
	void initGfx() {
	}

	// initializes compute (path tracing) pipeline
	void initCompute() {
		auto& dev = vulkanDevice();
		auto dsBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::compute)
		};

		comp_.dsLayout = {dev, dsBindings};
		comp_.pipeLayout = {dev, {comp_.dsLayout}, {}};

		vpp::ShaderModule compShader(dev, sen_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = comp_.pipeLayout;
		cpi.stage.module = compShader;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;

		vk::Pipeline vkPipeline;
		vk::createComputePipelines(dev, {}, 1, cpi, nullptr, vkPipeline);
		comp_.pipe = {dev, vkPipeline};

		// ds
		comp_.ds = {dev.descriptorAllocator(), comp_.dsLayout};
		vpp::DescriptorSetUpdate dsu(comp_.ds);
		dsu.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		dsu.storage({{boxesBuf_.buffer(), boxesBuf_.offset(), boxesBuf_.size()}});
		dsu.storage({{{}, lightTex_.vkImageView(), vk::ImageLayout::general}});
		dsu.apply();
	}

	void initPathtracePipe() {
		auto& dev = vulkanDevice();

		auto dsBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(vk::DescriptorType::storageImage,
				vk::ShaderStageBits::fragment)
		};

		pt_.dsLayout = {dev, dsBindings};
		pt_.pipeLayout = {dev, {pt_.dsLayout}, {}};

		vpp::ShaderModule fullscreenShader(dev, fullscreen_vert_data);
		vpp::ShaderModule textureShader(dev, senpt_frag_data);
		auto rp = renderer().renderPass();
		vpp::GraphicsPipelineInfo pipeInfo(rp, pt_.pipeLayout, {{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}}, 0, renderer().samples());

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},  1, pipeInfo.info(),
			nullptr, vkpipe);
		pt_.pipe = {dev, vkpipe};

		// box images
		std::uint32_t width = 6 * faceWidth;
		std::uint32_t height = boxes_.size() * faceHeight;
		atlasSize_ = {width, height};

		auto imgi = vpp::ViewableImageCreateInfo::color(
			dev, vk::Extent3D {width, height, 1u}).value();
		imgi.img.flags = vk::ImageCreateBits::mutableFormat;
		imgi.img.usage = vk::ImageUsageBits::transferDst |
			vk::ImageUsageBits::transferSrc |
			vk::ImageUsageBits::storage |
			vk::ImageUsageBits::sampled;

		// imgi.img.format = vk::Format::r32g32b32a32Sfloat;
		// imgi.view.format = vk::Format::r32g32b32a32Sfloat;
		imgi.img.format = vk::Format::r32Uint;
		imgi.view.format = vk::Format::r32Uint;

		// TODO: we could create second view with r8g8b8a8 format
		// and then use linear interpolation in sampler
		// mutable format already set
		lightTex_ = {dev, imgi};

		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		vpp::changeLayout(cb, lightTex_.image(), vk::ImageLayout::undefined,
			vk::PipelineStageBits::topOfPipe, {}, vk::ImageLayout::general,
			vk::PipelineStageBits::transfer,
			vk::AccessBits::memoryWrite, // not sure what clear image is
			{vk::ImageAspectBits::color, 0, 1, 0, 1u});

		auto clearValue = vk::ClearColorValue{};
		clearValue.uint32 = {0x44444444u, 0u, 0u, 0u};
		auto range = vk::ImageSubresourceRange {};
		range.aspectMask = vk::ImageAspectBits::color;
		range.baseArrayLayer = 0;
		range.layerCount = 1u;
		range.levelCount = 1;
		vk::cmdClearColorImage(cb, lightTex_.image(), vk::ImageLayout::general,
			clearValue, {range});

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
		auto& dev = vulkanDevice();
		auto mem = dev.hostMemoryTypes();

		// TODO: use real size instead of larger dummy
		ubo_ = {dev.bufferAllocator(), sizeof(float) * 128,
			vk::BufferUsageBits::uniformBuffer, 0, mem};

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
		pipeLayout_ = {dev, {dsLayout_}, {{vk::ShaderStageBits::fragment, 0, 4u}}};

		vpp::ShaderModule fullscreenShader(dev, fullscreen_vert_data);
		vpp::ShaderModule textureShader(dev, sen_frag_data);
		auto rp = renderer().renderPass();
		vpp::GraphicsPipelineInfo pipeInfo(rp, pipeLayout_, {{
			{fullscreenShader, vk::ShaderStageBits::vertex},
			{textureShader, vk::ShaderStageBits::fragment}
		}}, 0, renderer().samples());

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},  1, pipeInfo.info(),
			nullptr, vkpipe);
		pipe_ = {dev, vkpipe};

		// boxes
		// auto sizePerBox = 2 * sizeof(nytl::Mat4f) + sizeof(nytl::Vec4f);
		auto sizePerBox = sizeof(nytl::Mat4f) + sizeof(nytl::Vec4f);
		auto size = boxes_.size() * sizePerBox;
		boxesBuf_ = {dev.bufferAllocator(), size,
			vk::BufferUsageBits::storageBuffer, 0, dev.hostMemoryTypes()};

		auto map = boxesBuf_.memoryMap();
		auto span = map.span();
		for(auto& b : boxes_) {
			doi::write(span, b.box.inv);
			doi::write(span, b.color);

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
		auto& dev = vulkanDevice();
		auto rp = renderer().renderPass();

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
		rasterPipeLayout_ = {dev, {rasterSceneDsLayout_, rasterObjectDsLayout_}, {}};

		// pipe
		vpp::ShaderModule vertShader(dev, senr_vert_data);
		vpp::ShaderModule fragShader(dev, senr_frag_data);
		vpp::GraphicsPipelineInfo gpi(rp, rasterPipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment}
		}}, 0, renderer().samples());

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
			0, dev.deviceMemoryTypes()};

		// fill
		auto stage = vpp::SubBuffer{dev.bufferAllocator(), boxModelSize,
			vk::BufferUsageBits::transferSrc, 0u, dev.hostMemoryTypes()};
		auto map = stage.memoryMap();
		auto span = map.span();

		// write indices
		for(auto i = 0u; i < 24; i += 4) {
			doi::write<std::uint32_t>(span, i + 0);
			doi::write<std::uint32_t>(span, i + 1);
			doi::write<std::uint32_t>(span, i + 2);

			doi::write<std::uint32_t>(span, i + 0);
			doi::write<std::uint32_t>(span, i + 2);
			doi::write<std::uint32_t>(span, i + 3);
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

			doi::write(span, n - x + y); doi::write(span, n);
			doi::write(span, n + x + y); doi::write(span, n);
			doi::write(span, n + x - y); doi::write(span, n);
			doi::write(span, n - x - y); doi::write(span, n);

			// other (mirrored) side
			n *= -1.f;
			doi::write(span, n - x - y); doi::write(span, n);
			doi::write(span, n + x - y); doi::write(span, n);
			doi::write(span, n + x + y); doi::write(span, n);
			doi::write(span, n - x + y); doi::write(span, n);
		}

		// upload
		auto& qs = dev.queueSubmitter();
		auto cb = qs.device().commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});
		vk::BufferCopy region;
		region.dstOffset = boxModel_.offset();
		region.srcOffset = stage.offset();
		region.size = boxModelSize;
		vk::cmdCopyBuffer(cb, stage.buffer(), boxModel_.buffer(), {region});
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
			vk::BufferUsageBits::uniformBuffer, 0, dev.hostMemoryTypes()};

		// scene ds
		rasterSceneDs_ = {dev.descriptorAllocator(), rasterSceneDsLayout_};
		vpp::DescriptorSetUpdate sdsu(rasterSceneDs_);
		sdsu.uniform({{rasterSceneUbo_.buffer(),
			rasterSceneUbo_.offset(), rasterSceneUbo_.size()}});
		vpp::apply({sdsu});

		// boxes
		auto bufSize = 2 * sizeof(nytl::Mat4f) + sizeof(nytl::Vec4f)
			+ sizeof(std::uint32_t);
		auto i = 0u;
		for(auto& b: boxes_) {
			b.rasterdata = {dev.bufferAllocator(), bufSize,
				vk::BufferUsageBits::uniformBuffer, 0, dev.hostMemoryTypes()};
			{
				auto map = b.rasterdata.memoryMap();
				auto span = map.span();
				doi::write(span, b.box.transform);
				auto nm = nytl::Mat4f(transpose(b.box.inv));
				doi::write(span, nm);
				doi::write(span, b.color);
				doi::write(span, i);
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

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			doi::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
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

	void beforeRender(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute,
			comp_.pipe);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			comp_.pipeLayout, 0, {comp_.ds}, {});
		vk::cmdDispatch(cb, 256, 256, 1);
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, rasterPipe_);
		vk::cmdBindIndexBuffer(cb, boxModel_.buffer(),
			boxModel_.offset(), vk::IndexType::uint32);
		vk::cmdBindVertexBuffers(cb, 0, 1, {boxModel_.buffer()},
			{boxModel_.offset() + 36 * sizeof(std::uint32_t)});
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			rasterPipeLayout_, 0, {rasterSceneDs_}, {});

		for(auto& b : boxes_) {
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				rasterPipeLayout_, 1, {b.ds}, {});
			vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
		}
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == ny::Keycode::k0) {
			renderMode_ = 0;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == ny::Keycode::k1) {
			renderMode_ = 1;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == ny::Keycode::k2) {
			renderMode_ = 2;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == ny::Keycode::k3) {
			renderMode_ = 3;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == ny::Keycode::r) {
			renderMode_ = (renderMode_ + 1) % 4;
			App::scheduleRerecord();
			return true;
		} else if(ev.keycode == ny::Keycode::l) {
			camera_.update = true; // updates ubo
			if(ev.modifiers & ny::KeyboardModifier::shift) {
				showLightTex_ = (showLightTex_ + 2) % 3;
			} else {
				showLightTex_ = (showLightTex_ + 1) % 3;
			}
			return true;
		} else if(ev.keycode == ny::Keycode::i) {
			// save image
			saveImage_ = true;
		}

		return false;
	}

	void update(double delta) override {
		App::update(delta);
		App::scheduleRedraw(); // TODO: can be optimized
		time_ += delta;

		// movement
		auto kc = appContext().keyboardContext();
		auto fac = delta;

		auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
		auto right = nytl::normalized(nytl::cross(camera_.dir, yUp));
		auto up = nytl::normalized(nytl::cross(camera_.dir, right));
		if(kc->pressed(ny::Keycode::d)) { // right
			camera_.pos += fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::a)) { // left
			camera_.pos += -fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::w)) {
			camera_.pos += fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::s)) {
			camera_.pos += -fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::q)) { // up
			camera_.pos += -fac * up;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::e)) { // down
			camera_.pos += fac * up;
			camera_.update = true;
		}
	}

	void updateDevice() override {
		// raytracing
		auto rmap = ubo_.memoryMap();
		auto rspan = rmap.span();

		doi::write(rspan, camera_.pos);
		doi::write(rspan, 0.f); // padding
		doi::write(rspan, camera_.dir);
		doi::write(rspan, 0.f); // padding
		doi::write(rspan, camera_.perspective.fov);
		doi::write(rspan, camera_.perspective.aspect);
		doi::write(rspan, float(window().size().x));
		doi::write(rspan, float(window().size().y));
		doi::write(rspan, float(time_));

		// update scene ubo
		if(camera_.update) {
			camera_.update = false;

			// raster
			{
				auto map = rasterSceneUbo_.memoryMap();
				auto span = map.span();
				doi::write(span, matrix(camera_));
				doi::write(span, camera_.pos);
				doi::write(span, showLightTex_);
			}
		}

		// save texture
		if(saveImage_) {
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
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	bool needsDepth() const override {
		return true;
	}

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

	doi::Camera camera_;
	bool rotateView_ {};
	float time_ {};
	std::uint32_t showLightTex_ {0};

	int renderMode_ {3};
	nytl::Vec2ui atlasSize_;
	bool saveImage_ {false};
};

int main(int argc, const char** argv) {
	Lith app;
	if(!app.init({"lith", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}

