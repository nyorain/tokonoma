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
#include <katachi/stroke.hpp>

using namespace tkn::types;

class DummyApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	bool init(const nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		// counter clockwise (in standard-math coords)
		// auto points = std::array {
		// 	Vec2f{1.f, 0.f},
		// 	Vec2f{0.f, 1.f},
		// 	Vec2f{0.f, 0.f},
		// };
		// dlg_assert(ktc::area(points) > 0.0);

		// context
		rvg2::ContextSettings ctxs;
		ctxs.deviceFeatures = features_;
		ctxs.renderPass = renderPass();
		ctxs.uploadQueueFamily = vkDevice().queueSubmitter().queue().family();
		ctxs.subpass = 0u;
		ctxs.samples = vk::SampleCountBits::e1;

		context_ = std::make_unique<rvg2::Context>(vkDevice(), ctxs);

		// scene setup
		auto& uc = context_->updateContext();
		transformPool_.init(uc);
		clipPool_.init(uc);
		paintPool_.init(uc);
		vertexPool_.init(uc);
		indexPool_.init(uc);
		drawPool_.init(uc);

		// auto clip = rvg2::rectClip({-0.9, -0.9}, {1.8, 1.8});
		// clip_ = {*clipPool_, clip};
		// transform_ = {*transformPool_};

		{
			rvg2::PaintData paintData {};
			paintData.inner = {1.f, 0.5f, 0.8f, 1.f};
			paintData.transform[3][3] = 1u;
			paint0_ = {paintPool_, paintData};
		}

		{
			rvg2::PaintData paintData {};
			paintData.inner = {0.8f, 1.0f, 0.7f, 1.f};
			paintData.transform[3][3] = 1u;
			paint1_ = {paintPool_, paintData};
		}

		dlg_info("paint pool: {}", (rvg2::DeviceObject*) &paintPool_);

		rvg2::DrawMode dm;
		dm.fill = true;
		dm.stroke = 20.f;
		// dm.aaFill = true;
		dm.loop = true;
		dm.strokeExtrude = 0.9f;

		{
			std::array<Vec2f, 3> verts;
			/*
			verts[0] = {80.f, 100.f};
			verts[1] = {100.f, 350.f};
			verts[2] = {320.f, 300.f};
			verts[3] = {300.f, 100.f};
			*/
			verts[0] = {80.f, 100.f};
			verts[1] = {80.f, 110.f};
			verts[2] = {300.f, 100.f};
			draw0_ = {indexPool_, vertexPool_};
			draw0_.update(verts, dm);
		}

		{
			std::array<Vec2f, 6> verts;
			verts[0] = {600.f, 600.f};
			verts[1] = {600.f, 900.f};
			verts[2] = {900.f, 900.f};
			verts[3] = {900.f, 600.f};
			verts[4] = {750.f, 600.f};
			verts[5] = {650.f, 600.f};
			draw1_ = {indexPool_, vertexPool_};
			draw1_.update(verts, dm);
		}

		return true;
	}

	void update(double dt) override {
		Base::update(dt);
		Base::scheduleRedraw();

		time_ += dt;

		auto& paint0 = paint0_.changeData();
		paint0.inner[0] = 0.75 + 0.25 * std::sin(time_);
		paint0.inner[1] = 0.75 - 0.25 * std::cos(time_);

		if(rebatch_) {
			dlg_trace("rebatch");
			auto rec = rvg2::DrawRecorder(drawPool_, drawCalls_, drawDescriptors_);
			draw(rec);
			rebatch_ = false;
		}
	}

	void draw(rvg2::DrawRecorder& rec) {
		rec.bind(paint0_);
		draw1_.fill(rec);

		rec.bind(paint1_);
		draw0_.fill(rec);

		rec.bind(paint0_);
		draw0_.stroke(rec);

		rec.bind(paint1_);
		draw1_.stroke(rec);
	}

	void updateDevice() override {
		auto& uc = context_->updateContext();
		auto flags = uc.updateDevice();
		bool rerec = false;

		/*
		if(bool(flags & rvg2::UpdateFlags::rebatch)) {

			// TODO: ugly
			for(auto& call : drawCalls_) {
				rerec |= call.checkRerecord();
			}

			// TODO: ugly
			flags |= drawPool_.indirectCmdBuf.updateDevice();
			flags |= drawPool_.bindingsCmdBuf.updateDevice();
			dlg_assert(drawPool_.bindingsCmdBuf.buffer().size());
		}
		*/

		if(bool(flags & rvg2::UpdateFlags::descriptors)) {
			for(auto& ds : drawDescriptors_) {
				dlg_trace("updating descriptors");
				flags |= ds.updateDevice();
			}
		}

		if(rerec || bool(flags & rvg2::UpdateFlags::rerec)) {
			Base::scheduleRerecord();
		}

		vk::SubmitInfo si;
		auto sem = uc.endFrameSubmission(si);
		if(sem) {
			auto& qs = vkDevice().queueSubmitter();
			qs.add(si);
			addSemaphore(sem, vk::PipelineStageBits::allGraphics);
		}

		Base::updateDevice();
	}

	void render(vk::CommandBuffer cb) override {
		dlg_trace("render");
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

	rvg2::DrawPool drawPool_;
	rvg2::TransformPool transformPool_;
	rvg2::ClipPool clipPool_;
	rvg2::PaintPool paintPool_;
	rvg2::VertexPool vertexPool_;
	rvg2::IndexPool indexPool_;

	std::vector<rvg2::DrawCall> drawCalls_;
	std::vector<rvg2::DrawDescriptor> drawDescriptors_;

	vk::PhysicalDeviceDescriptorIndexingFeaturesEXT descriptorIndexingFeatures_;

	rvg2::Clip clip_;
	rvg2::Transform transform_;
	rvg2::Paint paint0_;
	rvg2::Paint paint1_;

	double time_ {0.0};
	bool rebatch_ {true};

	rvg2::Polygon draw0_;
	rvg2::Polygon draw1_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<DummyApp>(argc, argv);
}
