#include "context.hpp"
#include "scene.hpp"
#include <tkn/singlePassApp.hpp>
#include <tkn/types.hpp>
#include <tkn/shader.hpp>
#include <tkn/render.hpp>
#include <tkn/features.hpp>
#include <tkn/pipeline.hpp>
#include <dlg/dlg.hpp>
#include <nytl/matOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/queue.hpp>
#include <vpp/vk.hpp>

using namespace tkn::types;

class DummyApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	bool init(const nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		// context
		rvg2::ContextSettings ctxs;
		ctxs.deviceFeatures = rvg2::DeviceFeature::uniformDynamicArrayIndexing;
		ctxs.renderPass = renderPass();
		ctxs.uploadQueueFamily = vkDevice().queueSubmitter().queue().family();
		ctxs.subpass = 0u;
		ctxs.samples = vk::SampleCountBits::e1;

		context_ = std::make_unique<rvg2::Context>(vkDevice(), ctxs);

		// scene setup
		auto& ctx = *context_;
		scene_ = std::make_unique<rvg2::Scene>(ctx);
		transformPool_ = std::make_unique<rvg2::TransformPool>(ctx);
		clipPool_ = std::make_unique<rvg2::ClipPool>(ctx);
		paintPool_ = std::make_unique<rvg2::PaintPool>(ctx);
		vertexPool_ = std::make_unique<rvg2::VertexPool>(ctx);
		indexPool_ = std::make_unique<rvg2::IndexPool>(ctx);

		auto plane = nytl::Vec3f{1, 0, -10000};
		clipID_ = clipPool_->create(plane);

		transformID_ = transformPool_->create(nytl::identity<4, float>());
		// auto transform = nytl::identity<4, float>();
		// transformPool_->write(transformID_, transform);
		// transformPool_->writable(transformID_) = nytl::identity<4, float>();

		{
			std::array<rvg2::Vertex, 4> verts;
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

			rvg2::PaintPool::PaintData paintData {};
			paintData.inner = {1.f, 0.1f, 0.8f, 1.f};
			paintData.transform[3][3] = 1u;
			draw0_.paintID = paintPool_->create(paintData);
		}

		{
			std::array<rvg2::Vertex, 4> verts;
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

			rvg2::PaintPool::PaintData paintData {};
			paintData.inner = {0.8f, 0.8f, 0.5f, 1.f};
			paintData.transform[3][3] = 1u;
			draw1_.paintID = paintPool_->create(paintData);
		}

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
			rec.bindFontAtlas(context_->dummyImageView());

			for(auto& draw : {draw0_, draw1_}) {
				rvg2::DrawCall call;
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

		vk::SubmitInfo si;
		auto sem = context_->endFrameSubmit(si);
		if(sem) {
			auto& qs = vkDevice().queueSubmitter();
			qs.add(si);
			addSemaphore(sem, vk::PipelineStageBits::allGraphics);
		}

		Base::updateDevice();
	}

	void render(vk::CommandBuffer cb) override {
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
	std::unique_ptr<rvg2::Context> context_;
	std::unique_ptr<rvg2::Scene> scene_;
	std::unique_ptr<rvg2::TransformPool> transformPool_;
	std::unique_ptr<rvg2::ClipPool> clipPool_;
	std::unique_ptr<rvg2::PaintPool> paintPool_;
	std::unique_ptr<rvg2::VertexPool> vertexPool_;
	std::unique_ptr<rvg2::IndexPool> indexPool_;

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
