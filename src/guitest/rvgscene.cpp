#include "scene.hpp"
#include <tkn/singlePassApp.hpp>
#include <tkn/types.hpp>
#include <tkn/shader.hpp>
#include <tkn/render.hpp>
#include <tkn/features.hpp>
#include <tkn/pipeline.hpp>
#include <dlg/dlg.hpp>
#include <nytl/matOps.hpp>
#include <rvg/context.hpp>
#include <rvg/font.hpp>
#include <vpp/vk.hpp>

using namespace tkn::types;

class DummyApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	bool init(const nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		rvgInit();

		// scene setup
		scene_ = std::make_unique<rvg::Scene>(rvgContext());
		transformPool_ = std::make_unique<rvg::TransformPool>(rvgContext());
		clipPool_ = std::make_unique<rvg::ClipPool>(rvgContext());
		paintPool_ = std::make_unique<rvg::PaintPool>(rvgContext());
		vertexPool_ = std::make_unique<rvg::VertexPool>(rvgContext());
		indexPool_ = std::make_unique<rvg::IndexPool>(rvgContext());

		auto plane = nytl::Vec3f{1, 0, -10000};
		clipID_ = clipPool_->create(plane);

		transformID_ = transformPool_->create(nytl::identity<4, float>());
		// auto transform = nytl::identity<4, float>();
		// transformPool_->write(transformID_, transform);
		// transformPool_->writable(transformID_) = nytl::identity<4, float>();

		{
			std::array<rvg::Vertex, 4> verts;
			verts[0] = {{0.5f, 0.5f}, {0.f, 0.f}, {}};
			verts[1] = {{0.5f, 0.75f}, {0.f, 0.f}, {}};
			verts[2] = {{0.75f, 0.75f}, {0.f, 0.f}, {}};
			verts[3] = {{0.75f, 0.5f}, {0.f, 0.f}, {}};
			draw0_.vertexID = vertexPool_->create(verts);

			auto off = draw0_.vertexID;
			std::array<u32, 6> inds = {0, 1, 2, 0, 2, 3};
			for(auto& ind : inds) {
				ind += off;
			}
			draw0_.indexID = indexPool_->create(inds);

			rvg::PaintPool::PaintData paintData {};
			paintData.inner = {1.f, 0.1f, 0.8f, 1.f};
			paintData.transform[3][3] = 1u;
			draw0_.paintID = paintPool_->create(paintData);
		}

		{
			std::array<rvg::Vertex, 4> verts;
			verts[0] = {{-0.5f, -0.5f}, {0.f, 0.f}, {}};
			verts[1] = {{-0.5f, -0.75f}, {0.f, 0.f}, {}};
			verts[2] = {{-0.75f, -0.75f}, {0.f, 0.f}, {}};
			verts[3] = {{-0.75f, -0.5f}, {0.f, 0.f}, {}};
			draw1_.vertexID = vertexPool_->create(verts);

			auto off = draw1_.vertexID;
			std::array<u32, 6> inds = {0, 1, 2, 0, 2, 3};
			for(auto& ind : inds) {
				ind += off;
			}
			draw1_.indexID = indexPool_->create(inds);

			rvg::PaintPool::PaintData paintData {};
			paintData.inner = {0.8f, 0.8f, 0.5f, 1.f};
			paintData.transform[3][3] = 1u;
			draw1_.paintID = paintPool_->create(paintData);
		}

		// setup pipeline
		auto& dev = vkDevice();
		auto& sc = tkn::ShaderCache::instance(dev);
		auto preamble = "#define MULTIDRAW\n";
		auto vert = sc.load("guitest/fill2.vert", preamble).mod;
		auto frag = sc.load("guitest/fill2.frag", preamble).mod;

		vpp::GraphicsPipelineInfo gpi(renderPass(), scene_->pipeLayout(), {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment}
		}}}, 0u, samples());

		using Vertex = rvg::Vertex;
		auto vertexInfo = tkn::PipelineVertexInfo{{
				{0, 0, vk::Format::r32g32Sfloat, offsetof(Vertex, pos)},
				{1, 0, vk::Format::r32g32Sfloat, offsetof(Vertex, uv)},
				{2, 0, vk::Format::r8g8b8a8Unorm, offsetof(Vertex, color)},
			}, {
				{0, sizeof(Vertex), vk::VertexInputRate::vertex},
			}
		};

		gpi.vertex = vertexInfo.info();
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		pipe_ = {dev, gpi.info()};

		return true;
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();

		// record scene
		if(updated_) {
			auto rec = scene_->record();
			rec.bind(*transformPool_);
			rec.bind(*clipPool_);
			rec.bind(*vertexPool_);
			rec.bind(*indexPool_);
			rec.bind(*paintPool_);
			// rec.bindFontAtlas(rvgContext().defaultAtlas().texture().vkImageView());
			rec.bindFontAtlas(rvgContext().emptyImage().vkImageView());

			for(auto& draw : {draw0_, draw1_}) {
				rvg::DrawCall call;
				call.transform = transformID_;
				call.paint = draw.paintID;
				call.clipStart = clipID_;
				call.clipCount = 0; // TODO
				call.indexStart = draw.indexID;
				call.indexCount = 6u;
				call.type = 0u;
				call.uvFadeWidth = 0.0001f; // TODO
				rec.draw(call);
			}
		}

		time_ += dt;

		auto& paint0 = paintPool_->writable(draw0_.paintID);
		paint0.inner[0] = 0.75 + 0.25 * std::sin(time_);
		paint0.inner[1] = 0.75 - 0.25 * std::cos(time_);
	}

	void updateDevice() override {
		bool rerec = false;
		rerec |= clipPool_->updateDevice();
		rerec |= paintPool_->updateDevice();
		rerec |= transformPool_->updateDevice();
		rerec |= vertexPool_->updateDevice();
		rerec |= indexPool_->updateDevice();
		rerec |= scene_->updateDevice();
		updated_ = true;

		if(rerec) {
			Base::scheduleRerecord();
		}

		Base::updateDevice();
	}

	void render(vk::CommandBuffer cb) override {
		rvgContext().bindDefaults(cb);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		scene_->recordDraw(cb);
	}

	bool features(tkn::Features& enable, const tkn::Features& supported) override {
		if(!supported.base.features.multiDrawIndirect) {
			dlg_error("multidraw indirect not supported");
			return false;
		}

		enable.base.features.multiDrawIndirect = true;
		return true;
	}

	const char* name() const override { return "rvg-scene-test"; }

protected:
	vpp::Pipeline pipe_;

	std::unique_ptr<rvg::Scene> scene_;
	std::unique_ptr<rvg::TransformPool> transformPool_;
	std::unique_ptr<rvg::ClipPool> clipPool_;
	std::unique_ptr<rvg::PaintPool> paintPool_;
	std::unique_ptr<rvg::VertexPool> vertexPool_;
	std::unique_ptr<rvg::IndexPool> indexPool_;

	unsigned clipID_;
	unsigned transformID_;

	bool updated_ {false};
	double time_ {0.0};

	struct Draw {
		unsigned vertexID;
		unsigned indexID;
		unsigned paintID;
	};

	Draw draw0_;
	Draw draw1_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<DummyApp>(argc, argv);
}
