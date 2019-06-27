#include <stage/app.hpp>
#include <stage/window.hpp>
#include <stage/render.hpp>
#include <stage/camera.hpp>
#include <stage/bits.hpp>
#include <stage/scene/environment.hpp>
#include <stage/scene/shape.hpp>
#include <argagg.hpp>

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
#include <shaders/shv.sphere.vert.h>

class SHView : public doi::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!doi::App::init(args)) {
			return false;
		}

		// TODO: better pack coeffs (as vec4) in uniform buffer
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

		// data
		auto& qs = dev.queueSubmitter();
		auto cb = dev.commandAllocator().get(qs.queue().family());
		vk::beginCommandBuffer(cb, {});

		std::array<vpp::SubBuffer, 2> stages;
		if(skybox_) {
			stages[0] = initSkyPipe(cb);
		} else {
			stages = initSpherePipe(cb);
		}

		// ubo
		auto camUboSize = sizeof(nytl::Mat4f);
		cameraUbo_ = {dev.bufferAllocator(), camUboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// coeffs, ssbo
		auto cf = std::ifstream(file_);
		if(!cf.is_open()) {
			dlg_fatal("Can't open {}", file_);
			return false;
		}

		std::array<nytl::Vec3f, 9> coeffs;
		cf.read(reinterpret_cast<char*>(&coeffs), sizeof(coeffs));
		dlg_info("Spherical Harmonics coefficients:");
		for(auto i = 0u; i < coeffs.size(); ++i) {
			dlg_info("  coeffs[{}]: {}", i, coeffs[i]);
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

	[[nodiscard]] vpp::SubBuffer initSkyPipe(vk::CommandBuffer cb) {
		auto& dev = vulkanDevice();

		// pipeline
		vpp::ShaderModule vertShader(dev, stage_skybox_vert_data);
		vpp::ShaderModule fragShader(dev, shv_shv_frag_data);

		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples());

		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		pipe_ = {dev, gpi.info()};

		// indices
		auto usage = vk::BufferUsageBits::indexBuffer |
			vk::BufferUsageBits::transferDst;
		auto inds = doi::boxInsideIndices;
		indices_ = {dev.bufferAllocator(), sizeof(inds),
			usage, dev.deviceMemoryTypes(), 4u};
		auto boxIndicesStage = vpp::fillStaging(cb, indices_, inds);
		indexCount_ = 36;
		return boxIndicesStage;
	}

	[[nodiscard]] std::array<vpp::SubBuffer, 2>
	initSpherePipe(vk::CommandBuffer cb) {
		auto& dev = vulkanDevice();
		auto sphere = doi::Sphere{{0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}};
		auto shape = doi::generateUV(sphere);

		// pipeline
		vpp::ShaderModule vertShader(dev, shv_sphere_vert_data);
		vpp::ShaderModule fragShader(dev, shv_shv_frag_data);

		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples());

		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		vk::VertexInputAttributeDescription attrib;
		attrib.format = vk::Format::r32g32b32Sfloat;

		vk::VertexInputBindingDescription binding;
		binding.stride = sizeof(nytl::Vec3f);
		binding.inputRate = vk::VertexInputRate::vertex;

		gpi.vertex.pVertexAttributeDescriptions = &attrib;
		gpi.vertex.vertexAttributeDescriptionCount = 1;
		gpi.vertex.vertexBindingDescriptionCount = 1;
		gpi.vertex.pVertexBindingDescriptions = &binding;

		pipe_ = {dev, gpi.info()};

		// indices
		auto usage = vk::BufferUsageBits::indexBuffer |
			vk::BufferUsageBits::transferDst;
		auto inds = doi::bytes(shape.indices);
		indices_ = {dev.bufferAllocator(), std::size_t(inds.size()),
			usage, dev.deviceMemoryTypes(), 4u};
		auto indStage = vpp::fillStaging(cb, indices_, inds);
		indexCount_ = shape.indices.size();

		usage = vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::transferDst;
		auto poss = doi::bytes(shape.positions);
		spherePositions_ = {dev.bufferAllocator(), std::size_t(poss.size()),
			usage, dev.deviceMemoryTypes(), 4u};
		auto posStage = vpp::fillStaging(cb, spherePositions_, poss);

		indexCount_ = shape.indices.size();
		camera_.pos.z += 2.f;
		return {std::move(indStage), std::move(posStage)};
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		if(skybox_) {
			vk::cmdBindIndexBuffer(cb, indices_.buffer(),
				indices_.offset(), vk::IndexType::uint16);
		} else {
			vk::cmdBindVertexBuffers(cb, 0, {{spherePositions_.buffer()}},
				{{spherePositions_.offset()}});
			vk::cmdBindIndexBuffer(cb, indices_.buffer(),
				indices_.offset(), vk::IndexType::uint32);
		}
		vk::cmdDrawIndexed(cb, indexCount_, 1, 0, 0, 0);
	}

	void update(double dt) override {
		App::update(dt);
		auto kc = appContext().keyboardContext();
		if(kc) {
			doi::checkMovement(camera_, *kc, dt);
		}

		App::scheduleRedraw();
	}

	void updateDevice() override {
		App::updateDevice();

		if(camera_.update) {
			camera_.update = false;
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();
			doi::write(span, skybox_ ? fixedMatrix(camera_) : matrix(camera_));
			map.flush();
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

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	argagg::parser argParser() const override {
		auto parser = App::argParser();
		parser.definitions.push_back({
			"sphere", {"--sphere"},
			"Visualize as sphere instead of cubemap", 0});
		return parser;
	}

	bool handleArgs(const argagg::parser_results& result) override {
		if(!App::handleArgs(result)) {
			return false;
		}

		skybox_ = !result.has_option("sphere");
		if(result.pos.empty()) {
			dlg_fatal("No file argument given");
			return false;
		}

		file_ = result.pos[0];
		return true;
	}

	const char* name() const override { return "shview"; }
	bool needsDepth() const override { return true; } // for sphere
	const char* usageParams() const override { return "file [options]"; }

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;
	vpp::SubBuffer coeffs_;
	vpp::SubBuffer cameraUbo_;
	vpp::SubBuffer indices_;
	vpp::SubBuffer spherePositions_;
	bool rotateView_ {};
	bool skybox_ {};
	unsigned indexCount_ {};
	const char* file_ {};
	doi::Camera camera_ {};
};

int main(int argc, const char** argv) {
	SHView app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

