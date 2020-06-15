#include <tkn/singlePassApp.hpp>
#include <tkn/shader.hpp>
#include <tkn/ccam.hpp>
#include <tkn/bits.hpp>
#include <tkn/render.hpp>
#include <vpp/vk.hpp>
#include <vpp/shader.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>

#ifdef __ANDROID__
#include <shaders/tess.model.vert.h>
#include <shaders/tess.model.frag.h>
#include <shaders/tess.normal.vert.h>
#include <shaders/tess.normal.geom.h>
#include <shaders/tess.normal.frag.h>
#include <shaders/tess.model.tesc.h>
#include <shaders/tess.model.tese.h>
#endif

class TessApp : public tkn::SinglePassApp {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		auto& dev = vkDevice();
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
		};

		dsLayout_.init(dev, bindings);
		pipeLayout_ = {dev, {{dsLayout_}}, {}};

		auto uboSize = sizeof(nytl::Mat4f) + sizeof(nytl::Vec3f);
		ubo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes()};

		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{{ubo_}}});

		if(!createPipes()) {
			return false;
		}

		return true;
	}

	bool createPipes() {
		auto& dev = vkDevice();

#ifdef __ANDROID__
		auto modelVert = vpp::ShaderModule{dev, tess_model_vert_data};
		auto modelFrag = vpp::ShaderModule{dev, tess_model_frag_data};
		auto normalVert = vpp::ShaderModule{dev, tess_normal_vert_data};
		auto normalGeom = vpp::ShaderModule{dev, tess_normal_geom_data};
		auto normalFrag = vpp::ShaderModule{dev, tess_normal_frag_data};
		auto modelTesc = vpp::ShaderModule{dev, tess_model_tesc_data};
		auto modelTese = vpp::ShaderModule{dev, tess_model_tese_data};
#else
		auto pModelVert = tkn::loadShader(dev, "tess/model.vert");
		auto pModelFrag = tkn::loadShader(dev, "tess/model.frag");
		auto pNormalVert = tkn::loadShader(dev, "tess/normal.vert");
		auto pNormalGeom = tkn::loadShader(dev, "tess/normal.geom");
		auto pNormalFrag = tkn::loadShader(dev, "tess/normal.frag");
		auto pModelTesc = tkn::loadShader(dev, "tess/model.tesc");
		auto pModelTese = tkn::loadShader(dev, "tess/model.tese");
		if(!pModelVert || !pModelFrag || !pNormalVert || !pNormalGeom || !pNormalFrag) {
			dlg_error("Failed to load shader");
			return false;
		}

		auto modelVert = std::move(*pModelVert);
		auto modelFrag = std::move(*pModelFrag);
		auto normalVert = std::move(*pNormalVert);
		auto normalGeom = std::move(*pNormalGeom);
		auto normalFrag = std::move(*pNormalFrag);
		auto modelTesc = std::move(*pModelTesc);
		auto modelTese = std::move(*pModelTese);
#endif

		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{modelVert, vk::ShaderStageBits::vertex},
			{modelTesc, vk::ShaderStageBits::tessellationControl},
			{modelTese, vk::ShaderStageBits::tessellationEvaluation},
			{modelFrag, vk::ShaderStageBits::fragment},
		}}});

		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.assembly.topology = vk::PrimitiveTopology::patchList;
		gpi.rasterization.polygonMode = vk::PolygonMode::fill;
		gpi.tesselation.patchControlPoints = 4;
		modelPipe_ = {vkDevice(), gpi.info()};

		gpi.program = {{{
			{normalVert, vk::ShaderStageBits::vertex},
			{modelTesc, vk::ShaderStageBits::tessellationControl},
			{modelTese, vk::ShaderStageBits::tessellationEvaluation},
			{normalGeom, vk::ShaderStageBits::geometry},
			{normalFrag, vk::ShaderStageBits::fragment},
		}}};
		gpi.rasterization.polygonMode = vk::PolygonMode::line;
		normalPipe_ = {vkDevice(), gpi.info()};

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, modelPipe_);
		vk::cmdDraw(cb, 4, 1, 0, 0);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, normalPipe_);
		vk::cmdDraw(cb, 4, 1, 0, 0);
	}

	void update(double dt) override {
		App::update(dt);
		camera_.update(swaDisplay(), dt);
		if(camera_.needsUpdate) {
			App::scheduleRedraw();
		}
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.needsUpdate) {
			camera_.needsUpdate = false;

			auto map = ubo_.memoryMap();
			auto span = map.span();
			tkn::write(span, camera_.viewProjectionMatrix());
			tkn::write(span, camera_.position());
		}

		if(reload_) {
			createPipes();
			reload_ = false;
			App::scheduleRerecord();
		}
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		App::mouseMove(ev);
		camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	bool key(const swa_key_event& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == swa_key_r) {
#ifndef __ANDROID__
			reload_ = true;
			App::scheduleRedraw();
			return true;
#endif
		}

		return false;
	}

	void resize(unsigned w, unsigned h) override {
		App::resize(w, h);
		camera_.aspect({w, h});
	}

	const char* name() const override { return "tess"; }
	bool needsDepth() const override { return true; }

protected:
	vpp::Pipeline modelPipe_;
	vpp::Pipeline normalPipe_;
	vpp::PipelineLayout pipeLayout_;
	vpp::SubBuffer ubo_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;

	tkn::ControlledCamera camera_ {};
	bool reload_ {};
};

int main(int argc, const char** argv) {
	TessApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

