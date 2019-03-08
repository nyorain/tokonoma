#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/bits.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/bufferOps.hpp>

#include <nytl/mat.hpp>
#include <ny/mouseButton.hpp>
#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>

#include <shaders/fullscreen.vert.h>
#include <shaders/sen.frag.h> // XXX: make compute shader; write textures
#include <shaders/senr.vert.h>
#include <shaders/senr.frag.h>

#include "render.hpp"

class SenApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		initBoxes();
		initRaytracePipe();
		initRasterPipe();

		return true;
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
		auto sizePerBox = sizeof(nytl::Mat4f) + sizeof(nytl::Vec4f);
		auto size = boxes_.size() * sizePerBox;
		boxesBuf_ = {dev.bufferAllocator(), size,
			vk::BufferUsageBits::storageBuffer, 0, dev.hostMemoryTypes()};

		auto map = boxesBuf_.memoryMap();
		auto span = map.span();
		for(auto& b : boxes_) {
			doi::write(span, b.box.transform);
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

		// ds layout
		auto sceneBindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::fragment | vk::ShaderStageBits::vertex),
		};

		auto objectBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
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
			+ sizeof(nytl::Vec3f); // viewPos
		rasterSceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, dev.hostMemoryTypes()};

		// scene ds
		rasterSceneDs_ = {dev.descriptorAllocator(), rasterSceneDsLayout_};
		vpp::DescriptorSetUpdate sdsu(rasterSceneDs_);
		sdsu.uniform({{rasterSceneUbo_.buffer(),
			rasterSceneUbo_.offset(), rasterSceneUbo_.size()}});
		vpp::apply({sdsu});

		// boxes
		auto bufSize = 2 * sizeof(nytl::Mat4f) + sizeof(nytl::Vec4f);
		for(auto& b: boxes_) {
			b.rasterdata = {dev.bufferAllocator(), bufSize,
				vk::BufferUsageBits::uniformBuffer, 0, dev.hostMemoryTypes()};
			{
				auto map = b.rasterdata.memoryMap();
				auto span = map.span();
				doi::write(span, b.box.inv);
				auto nm = nytl::Mat4f(transpose(inverse(b.box.inv)));
				doi::write(span, nm);
				doi::write(span, b.color);
			}

			b.ds = {dev.descriptorAllocator(), rasterSceneDsLayout_};
			vpp::DescriptorSetUpdate update(b.ds);
			update.uniform({{b.rasterdata.buffer(),
				b.rasterdata.offset(), b.rasterdata.size()}});
		}
	}

	void initBoxes() {
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
		boxes_.back().color = {1.f, 0.f, 0.f};

		boxes_.emplace_back(); // right
		boxes_.back().box = Box {{4.f, 0.f, 0.f},
			{0.1f, 0.f, 0.f},
			{0.f, 4.f, 0.f},
			{0.f, 0.f, 4.f},
		};
		boxes_.back().color = {0.f, 1.f, 0.f};

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
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			using nytl::constants::pi;
			yaw_ += 0.005 * ev.delta.x;
			pitch_ += 0.005 * ev.delta.y;
			pitch_ = std::clamp<float>(pitch_, -pi / 2 + 0.1, pi / 2 - 0.1);

			camera_.dir.x = std::sin(yaw_) * std::cos(pitch_);
			camera_.dir.y = -std::sin(pitch_);
			camera_.dir.z = -std::cos(yaw_) * std::cos(pitch_);
			nytl::normalize(camera_.dir);
			camera_.update = true;
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

	void render(vk::CommandBuffer cb) override {
		if(rasterize_) {
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
		} else {
			vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
			vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
				pipeLayout_, 0, {ds_.vkHandle()}, {});
			vk::cmdDraw(cb, 4, 1, 0, 0);
		}
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == ny::Keycode::r) {
			rasterize_ = !rasterize_;
			rerecord();
			return true;
		}

		return false;
	}

	void update(double delta) override {
		App::update(delta);
		App::redraw(); // TODO: can be optimized

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
		// update scene ubo
		if(camera_.update) {
			camera_.update = false;

			// raytracing
			{
				auto map = ubo_.memoryMap();
				auto span = map.span();

				doi::write(span, camera_.pos);
				doi::write(span, 0.f); // padding
				doi::write(span, camera_.dir);
				doi::write(span, 0.f); // padding
				doi::write(span, fov_);
				doi::write(span, aspect_);
				doi::write(span, float(window().size().x));
				doi::write(span, float(window().size().y));
			}

			// raster
			{
				auto map = rasterSceneUbo_.memoryMap();
				auto span = map.span();

				auto mat = doi::perspective3RH<float>(fov_, aspect_, near_, far_);
				nytl::Vec3f up {0.f, 1.f, 0.f};
				mat = mat * doi::lookAtRH(camera_.pos,
					camera_.pos + camera_.dir, up);
				doi::write(span, mat);
				doi::write(span, camera_.pos);
			}
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		aspect_ = float(ev.size.x) / ev.size.y;
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

	// raster
	vpp::Pipeline rasterPipe_;
	vpp::PipelineLayout rasterPipeLayout_;
	vpp::TrDsLayout rasterSceneDsLayout_;
	vpp::TrDsLayout rasterObjectDsLayout_;
	vpp::TrDs rasterSceneDs_;
	vpp::SubBuffer boxModel_;
	vpp::SubBuffer rasterSceneUbo_;

	struct {
		nytl::Vec3f pos {0.f, 0.f, 0.f};
		nytl::Vec3f dir {0.f, 0.f, -1.f};
		nytl::Mat4f transform;
		bool update {true};
	} camera_;

	std::vector<RenderBox> boxes_;

	bool rotateView_ {};
	float fov_ {nytl::constants::pi * 0.4};
	float aspect_ {1.f};
	float near_ {0.1f};
	float far_ {100.f};
	float pitch_ {};
	float yaw_ {};

	bool rasterize_ {true};
};

int main(int argc, const char** argv) {
	// run app
	SenApp app;
	if(!app.init({"sen", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
