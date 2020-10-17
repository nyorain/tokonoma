#include "context.hpp"
#include "draw.hpp"
#include "polygon.hpp"
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
		ctxs.deviceFeatures = features_;
		ctxs.renderPass = renderPass();
		ctxs.uploadQueueFamily = vkDevice().queueSubmitter().queue().family();
		ctxs.subpass = 0u;
		ctxs.samples = vk::SampleCountBits::e1;

		context_ = std::make_unique<rvg2::Context>(vkDevice(), ctxs);

		// scene setup
		auto& ctx = *context_;
		transformPool_ = std::make_unique<rvg2::TransformPool>(ctx);
		clipPool_ = std::make_unique<rvg2::ClipPool>(ctx);
		paintPool_ = std::make_unique<rvg2::PaintPool>(ctx);
		vertexPool_ = std::make_unique<rvg2::VertexPool>(ctx);
		indexPool_ = std::make_unique<rvg2::IndexPool>(ctx);
		drawPool_ = std::make_unique<rvg2::DrawPool>(ctx);

		// auto clip = rvg2::rectClip({-0.9, -0.9}, {1.8, 1.8});
		// clip_ = {*clipPool_, clip};
		// transform_ = {*transformPool_};

		{
			rvg2::PaintData paintData {};
			paintData.inner = {1.f, 0.1f, 0.8f, 1.f};
			paintData.transform[3][3] = 1u;
			paint0_ = {*paintPool_, paintData};
		}

		{
			rvg2::PaintData paintData {};
			paintData.inner = {0.8f, 0.8f, 0.5f, 1.f};
			paintData.transform[3][3] = 1u;
			paint1_ = {*paintPool_, paintData};
		}

		rvg2::DrawMode dm;
		dm.fill = true;
		dm.stroke = 20.f;
		dm.aaStroke = true;
		dm.aaFill = true;
		dm.loop = true;

		{
			std::array<Vec2f, 4> verts;
			verts[0] = {100.f, 100.f};
			verts[1] = {100.f, 300.f};
			verts[2] = {300.f, 300.f};
			verts[3] = {300.f, 100.f};
			draw0_ = {*indexPool_, *vertexPool_};
			draw0_.update(verts, dm);
		}

		{
			std::array<Vec2f, 4> verts;
			verts[0] = {600.f, 600.f};
			verts[1] = {600.f, 900.f};
			verts[2] = {900.f, 900.f};
			verts[3] = {900.f, 600.f};
			draw1_ = {*indexPool_, *vertexPool_};
			draw1_.update(verts, dm);
		}

		// TODO
		auto id = drawPool_->indirectCmdBuf.allocate(100);
		drawPool_->indirectCmdBuf.free(id + 99, 1);
		id = drawPool_->bindingsCmdBuf.allocate(100);
		drawPool_->bindingsCmdBuf.free(id + 99, 1);

		return true;
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();

		// record scene
		if(updated_) {
			dlg_assert(vpp::BufferSpan(drawPool_->bindingsCmdBuf.buffer()).valid());

			auto rec = rvg2::DrawRecorder(*drawPool_, drawCalls_, drawDescriptors_);
			rec.bind(*vertexPool_);
			rec.bind(*indexPool_);
			rec.bindFontAtlas(context_->dummyImageView());

			// rec.bind(transform_);
			// rec.bind(clip_);

			rec.bind(paint0_);
			draw1_.fill(rec);
			draw0_.stroke(rec);

			rec.bind(paint1_);
			draw0_.fill(rec);
			draw1_.stroke(rec);

			updated_ = false;
		}

		time_ += dt;

		auto& paint0 = paint0_.changeData();
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
		rerec |= drawPool_->indirectCmdBuf.updateDevice();
		rerec |= drawPool_->bindingsCmdBuf.updateDevice();

		for(auto& call : drawCalls_) {
			rerec |= call.checkRerecord();
		}

		for(auto& ds : drawDescriptors_) {
			rerec |= ds.updateDevice();
		}

		if(rerec) {
			dlg_assert(vpp::BufferSpan(drawPool_->bindingsCmdBuf.buffer()).valid());

			// HACK: only really needed the first time...
			updated_ = true;
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
		const auto extent = swapchainInfo().imageExtent;
		rvg2::record(cb, drawCalls_, extent);
	}

	bool features(tkn::Features& enable, const tkn::Features& supported) override {
		if(supported.base.features.multiDrawIndirect) {
			enable.base.features.multiDrawIndirect = true;
			features_ |= rvg2::DeviceFeature::multidrawIndirect;
		}

		if(supported.base.features.shaderUniformBufferArrayDynamicIndexing) {
			enable.base.features.shaderUniformBufferArrayDynamicIndexing = true;
			features_ |= rvg2::DeviceFeature::uniformDynamicArrayIndexing;
		}

		if(supported.base.features.shaderClipDistance) {
			enable.base.features.shaderClipDistance = true;
			features_ |= rvg2::DeviceFeature::clipDistance;
		}

		if(hasDescriptorIndexing_) {
			auto checkEnable = [](auto& check, auto& set) {
				if(check) {
					set = true;
				}
			};

			checkEnable(
				supported.descriptorIndexing.descriptorBindingPartiallyBound,
				enable.descriptorIndexing.descriptorBindingPartiallyBound);
			checkEnable(
				supported.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind,
				enable.descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind);
			checkEnable(
				supported.descriptorIndexing.descriptorBindingStorageBufferUpdateAfterBind,
				enable.descriptorIndexing.descriptorBindingStorageBufferUpdateAfterBind);

			descriptorIndexingFeatures_ = enable.descriptorIndexing;
		}

		return true;
	}

	const char* name() const override { return "rvg-scene-test"; }

protected:
	rvg2::DeviceFeatureFlags features_ {};

	std::unique_ptr<rvg2::Context> context_;
	std::unique_ptr<rvg2::DrawPool> drawPool_;
	std::unique_ptr<rvg2::TransformPool> transformPool_;
	std::unique_ptr<rvg2::ClipPool> clipPool_;
	std::unique_ptr<rvg2::PaintPool> paintPool_;
	std::unique_ptr<rvg2::VertexPool> vertexPool_;
	std::unique_ptr<rvg2::IndexPool> indexPool_;

	std::vector<rvg2::DrawCall> drawCalls_;
	std::vector<rvg2::DrawDescriptor> drawDescriptors_;

	vk::PhysicalDeviceDescriptorIndexingFeaturesEXT descriptorIndexingFeatures_;

	rvg2::Clip clip_;
	rvg2::Transform transform_;
	rvg2::Paint paint0_;
	rvg2::Paint paint1_;

	bool updated_ {false};
	double time_ {0.0};

	rvg2::Polygon draw0_;
	rvg2::Polygon draw1_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<DummyApp>(argc, argv);
}
