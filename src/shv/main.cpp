#include <stage/app.hpp>
#include <stage/window.hpp>
#include <stage/render.hpp>
#include <stage/camera.hpp>
#include <stage/bits.hpp>
#include <stage/scene/environment.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/vk.hpp>

#include <dlg/dlg.hpp>
#include <ny/mouseButton.hpp>
#include <ny/appContext.hpp>
#include <ny/keyboardContext.hpp>

#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>

#include <fstream>
#include <shaders/stage.skybox.vert.h>
#include <shaders/shv.shv.frag.h>

class SHView : public doi::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!doi::App::init(args)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto bindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::fragment),
		};

		dsLayout_ = {dev, bindings};
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};

		// pipeline
		vpp::ShaderModule vertShader(dev, stage_skybox_vert_data);
		vpp::ShaderModule fragShader(dev, shv_shv_frag_data);

		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}});

		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		pipe_ = {dev, gpi.info()};

		// data
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		// indices
		auto usage = vk::BufferUsageBits::indexBuffer |
			vk::BufferUsageBits::transferDst;
		auto inds = doi::boxInsideIndices;
		boxIndices_ = {dev.bufferAllocator(), sizeof(inds),
			usage, dev.deviceMemoryTypes(), 4u};
		auto boxIndicesStage = vpp::fillStaging(cb, boxIndices_, inds);

		// ubo
		auto camUboSize = sizeof(nytl::Mat4f);
		cameraUbo_ = {dev.bufferAllocator(), camUboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// coeffs, ssbo
		auto cf = std::ifstream("sh.bin");
		if(!cf.is_open()) {
			dlg_fatal("Can't open sh.bin");
			return false;
		}

		std::array<nytl::Vec3f, 9> coeffs;
		cf.read(reinterpret_cast<char*>(&coeffs), sizeof(coeffs));
		for(auto i = 0u; i < coeffs.size(); ++i) {
			dlg_info("coeffs[{}]: {}", i, coeffs[i]);
		}

		coeffs_ = {dev.bufferAllocator(), sizeof(coeffs),
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferDst, dev.hostMemoryTypes()};
		auto coeffsStage = vpp::fillStaging(cb, coeffs_, coeffs);

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// ds
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{{cameraUbo_}}});
		dsu.storage({{{coeffs_}}});

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		vk::cmdBindIndexBuffer(cb, boxIndices_.buffer(),
			boxIndices_.offset(), vk::IndexType::uint16);
		vk::cmdDrawIndexed(cb, 36, 1, 0, 0, 0);
	}

	void update(double dt) override {
		App::update(dt);
		auto kc = appContext().keyboardContext();
		if(kc) {
			doi::checkMovement(camera_, *kc, dt);
		}
	}

	void updateDevice() override {
		App::updateDevice();

		if(camera_.update) {
			camera_.update = false;
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();
			doi::write(span, fixedMatrix(camera_));
		}
	}

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

	const char* name() const override { return "shview"; }

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;
	vpp::SubBuffer coeffs_;
	vpp::SubBuffer cameraUbo_;
	vpp::SubBuffer boxIndices_;
	bool rotateView_ {};
	doi::Camera camera_ {};
};

int main(int argc, const char** argv) {
	SHView app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

