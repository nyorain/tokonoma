#include "util.hpp"
#include <tkn/app.hpp>
#include <tkn/camera.hpp>
#include <tkn/transform.hpp>
#include <tkn/render.hpp>
#include <tkn/bits.hpp>
#include <tkn/window.hpp>
#include <tkn/types.hpp>
#include <nytl/vecOps.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <dlg/dlg.hpp>
#include <ny/mouseButton.hpp>
#include <ny/appContext.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/key.hpp>
#include <ny/event.hpp>
#include <ny/windowContext.hpp>
#include <ny/cursor.hpp>
#include <argagg.hpp>
#include <variant>

#include <shaders/bezier.point.vert.h>
#include <shaders/bezier.point.frag.h>
#include <shaders/bezier.line.frag.h>
#include <shaders/tkn.simple3.vert.h>

// TODO
// - use indirect drawing (see update)
// - allow to choose order of bezier via gui (or add points somehow)?
//   when increasing degree use bezier splitting
//   when decreasing, approximate via least squares

// ideas:
// - allow closed spline curves (e.g. via checkbox)
// - better b-spline subdivision. Use dynamic condition
// - implement nurbs instead of just b-splines
// - using tensor products, implement bezier surfaces
//   (-> also b-spline surfaces)
// - using triangular bezier curves, implement bezier surfaces

using namespace tkn::types;

class BezierApp : public tkn::App {
public:
	struct Drag {
		u32 id;
		float depth;
	};

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		bool spline = (controlPoints_.index() == 1);
		if(spline) {
			auto zero = Vec3f {0.f, 0.f, 0.f};
			controlPoints_ = std::vector<Vec3f>{
				zero, zero, zero, zero, zero, zero, zero
			};
		} else {
			controlPoints_ = Bezier<3>{{{
				{0.f, 0.f, 0.f},
				{0.f, 1.f, 0.f},
				{1.f, 0.f, 0.f},
				{1.f, 1.f, 0.f},
			}}};
		}

		auto& dev = vulkanDevice();
		auto bindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex),
		};

		dsLayout_ = {dev, bindings};

		vk::PushConstantRange pcr;
		pcr.stageFlags = vk::ShaderStageBits::fragment;
		pcr.size = sizeof(nytl::Vec4f);
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {{pcr}}};

		// pipeline
		vpp::ShaderModule pointVert(dev, bezier_point_vert_data);
		vpp::ShaderModule pointFrag(dev, bezier_point_frag_data);

		vpp::GraphicsPipelineInfo gpi(renderPass(), pipeLayout_, {{{
			{pointVert, vk::ShaderStageBits::vertex},
			{pointFrag, vk::ShaderStageBits::fragment},
		}}}, 0, samples());

		gpi.assembly.topology = vk::PrimitiveTopology::pointList;
		gpi.rasterization.polygonMode = vk::PolygonMode::point;
		// gpi.depthStencil.depthTestEnable = true;
		// gpi.depthStencil.depthWriteEnable = true;
		// gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		vk::VertexInputAttributeDescription attrib;
		attrib.format = vk::Format::r32g32b32Sfloat;

		vk::VertexInputBindingDescription binding;
		binding.stride = sizeof(nytl::Vec3f);
		binding.inputRate = vk::VertexInputRate::vertex;

		gpi.vertex.pVertexAttributeDescriptions = &attrib;
		gpi.vertex.vertexAttributeDescriptionCount = 1;
		gpi.vertex.vertexBindingDescriptionCount = 1;
		gpi.vertex.pVertexBindingDescriptions = &binding;

		pointPipe_ = {dev, gpi.info()};

		// line pipe
		vpp::ShaderModule lineVert(dev, tkn_simple3_vert_data);
		vpp::ShaderModule lineFrag(dev, bezier_line_frag_data);

		gpi = {renderPass(), pipeLayout_, {{{
			{lineVert, vk::ShaderStageBits::vertex},
			{lineFrag, vk::ShaderStageBits::fragment},
		}}}, 0, samples()};

		gpi.assembly.topology = vk::PrimitiveTopology::lineStrip;
		gpi.rasterization.polygonMode = vk::PolygonMode::line;
		// gpi.depthStencil.depthTestEnable = true;
		// gpi.depthStencil.depthWriteEnable = true;
		// gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
//
		gpi.vertex.pVertexAttributeDescriptions = &attrib;
		gpi.vertex.vertexAttributeDescriptionCount = 1;
		gpi.vertex.vertexBindingDescriptionCount = 1;
		gpi.vertex.pVertexBindingDescriptions = &binding;

		linePipe_ = {dev, gpi.info()};

		auto camUboSize = sizeof(nytl::Mat4f);
		cameraUbo_ = {dev.bufferAllocator(), camUboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		auto pointsSize = sizeof(nytl::Vec3f) * points().size();
		pointVerts_ = {dev.bufferAllocator(), pointsSize,
			vk::BufferUsageBits::vertexBuffer, dev.hostMemoryTypes()};

		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{{cameraUbo_}}});
		dsu.apply();

		return true;
	}

	void update(double dt) override {
		App::update(dt);
		auto kc = appContext().keyboardContext();
		if(kc) {
			if(tkn::checkMovement(camera_, *kc, dt)) {
				App::scheduleRedraw();
			}
		}

		if(updateVerts_) {
			if(auto* bezier = std::get_if<0>(&controlPoints_)) {
				flattened_ = subdivide(*bezier, 12, 0.00001f);
			} else {
				flattened_ = std::get<1>(controlPoints_);
				for(auto i = 0u; i < 4; ++i) {
					flattened_ = subdivide3(flattened_);
				}
			}

			App::scheduleRedraw();
		}
	}

	void updateDevice() override {
		App::updateDevice();

		if(camera_.update) {
			camera_.update = false;
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();
			tkn::write(span, matrix(camera_));
			map.flush();
		}

		if(updateVerts_) {
			updateVerts_ = false;
			nytl::Span<const std::byte> points = nytl::as_bytes(this->points());
			if(points.size() > pointVerts_.size()) {
				u32 size = points.size() * 2;
				pointVerts_ = {vulkanDevice().bufferAllocator(), size,
					vk::BufferUsageBits::vertexBuffer,
					vulkanDevice().hostMemoryTypes(), 8u};
				App::scheduleRerecord();
				dlg_info("recreated pointVerts_");
			}
			auto map = pointVerts_.memoryMap();
			auto span = map.span();
			tkn::write(span, points);
			map.flush();

			points = tkn::bytes(flattened_);
			if(u32(points.size()) > flatVerts_.size()) {
				u32 size = points.size() * 2;
				flatVerts_ = {vulkanDevice().bufferAllocator(), size,
					vk::BufferUsageBits::vertexBuffer,
					vulkanDevice().hostMemoryTypes(), 8u};
				App::scheduleRerecord();
				dlg_info("recreated flagVerts_");
			}

			map = flatVerts_.memoryMap();
			span = map.span();
			tkn::write(span, points);
			map.flush();
		}
	}

	nytl::Span<Vec3f> points() {
		if(auto* bezier = std::get_if<0>(&controlPoints_)) {
			return bezier->points;
		} else {
			auto& pts = std::get<1>(controlPoints_);
			return nytl::Span<Vec3f>(pts).subspan(3, pts.size() - 6);
		}
	}

	void render(vk::CommandBuffer cb) override {
		// control points
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pointPipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		vk::cmdBindVertexBuffers(cb, 0, {{pointVerts_.buffer()}},
			{{pointVerts_.offset()}});
		vk::cmdDraw(cb, points().size(), 1, 0, 0);

		// support outlines
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, linePipe_);
		nytl::Vec4f col = {0.f, 0.f, 0.f, 1.f};
		vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::fragment,
			0, sizeof(nytl::Vec4f), &col);
		vk::cmdDraw(cb, points().size(), 1, 0, 0);

		// real curve
		col = {0.1f, 0.1f, 0.1f, 1.f};
		vk::cmdPushConstants(cb, pipeLayout_, vk::ShaderStageBits::fragment,
			0, sizeof(nytl::Vec4f), &col);
		vk::cmdBindVertexBuffers(cb, 0, {{flatVerts_.buffer()}},
			{{flatVerts_.offset()}});
		vk::cmdDraw(cb, flattened_.size(), 1, 0, 0);
	}

	std::optional<Drag> pointAt(nytl::Vec2f pos) {
		using namespace nytl::vec::cw::operators;
		auto proj = matrix(camera_);

		// TODO: depth sort? in case there are multiple points
		// over each other. Not really a sort, just a "best
		// (optional) candidate" iteration
		auto points = this->points();
		for(auto i = 0u; i < points.size(); ++i) {
			auto ndc = tkn::multPos(proj, points[i]);
			ndc.y = -ndc.y;
			auto xy = 0.5f * nytl::Vec2f{ndc.x + 1, ndc.y + 1};
			auto screen = window().size() * xy;
			if(length(pos - screen) < 10.f) {
				return {{i, ndc.z}};
			}
		}

		return {};
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(drag_) {
			// unproject
			auto s = window().size();
			auto p = Vec2f(ev.position);
			auto xy = Vec2f{-1.f + 2 * p.x / s.x, -1.f + 2 * p.y / s.y};
			xy.y = -xy.y;
			auto invProj = nytl::Mat4f(nytl::inverse(matrix(camera_)));
			auto world = tkn::multPos(invProj, Vec3f{xy.x, xy.y, drag_->depth});
			points()[drag_->id] = world;
			if(auto* ppts = std::get_if<1>(&controlPoints_)) {
				auto& pts = *ppts;
				if(drag_->id == 0) {
					pts[0] = pts[1] = pts[2] = world;
				} else if(drag_->id == points().size() - 1) {
					pts[pts.size() - 3] = pts[pts.size() - 2] = pts[pts.size() - 1] = world;
				}
			}

			updateVerts_ = true;
			return;
		}

		if(rotateView_) {
			tkn::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
			App::scheduleRedraw();
		}

		if(auto i = pointAt(Vec2f(ev.position)); i) {
			window().windowContext().cursor(ny::Cursor::Type::hand);
		} else {
			window().windowContext().cursor(ny::Cursor::Type::leftPtr);
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			if(!ev.pressed) {
				drag_ = {};
				rotateView_ = false;
				return true;
			}

			drag_ = pointAt(Vec2f(ev.position));
			if(drag_) {
				window().windowContext().cursor(ny::Cursor::Type::hand);
			} else {
				rotateView_ = ev.pressed;
			}

			return true;
		}

		return false;
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	std::vector<vk::ClearValue> clearValues() override {
		std::vector<vk::ClearValue> clearValues;
		clearValues.reserve(3);
		vk::ClearValue c {{0.7f, 0.7f, 0.7f, 1.f}};

		clearValues.push_back(c); // clearColor (value unused for msaa)
		if(samples() != vk::SampleCountBits::e1) { // msaa attachment
			clearValues.push_back({c});
		}

		if(depthFormat() != vk::Format::undefined && needsDepth()) {
			clearValues.emplace_back(c).depthStencil = {1.f, 0u};
		}

		return clearValues;
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == ny::Keycode::c) {
			if(auto* ppts = std::get_if<1>(&controlPoints_)) {
				auto& pts = *ppts;
				auto p = camera_.pos + camera_.dir;
				pts.push_back(p);
				pts[pts.size() - 2] = p;
				pts[pts.size() - 3] = p;
				pts[pts.size() - 4] = p;
				updateVerts_ = true;

				// TODO: with indirect drawing we could get rid of this
				App::scheduleRerecord();
			}
		}

		return false;
	}

	argagg::parser argParser() const override {
		auto parser = App::argParser();
		parser.definitions.push_back({
			"no-spline", {"--no-spline", "-b"},
			"Use a simple bezier curve instead of a B-Spline", 0
		});
		return parser;
	}

	bool handleArgs(const argagg::parser_results& result) override {
		if(!App::handleArgs(result)) {
			return false;
		}

		if(result.has_option("no-spline")) {
			controlPoints_ = Bezier<3> {};
		} else {
			controlPoints_ = std::vector<Vec3f> {};
		}

		return true;
	}

	const char* name() const override { return "bezier"; }
	bool needsDepth() const override { return false; }

protected:
	tkn::Camera camera_;
	std::variant<Bezier<3>, std::vector<Vec3f>> controlPoints_;
	bool rotateView_ {};
	std::vector<Vec3f> flattened_;

	std::optional<Drag> drag_ {};
	bool updateVerts_ {true};
	vpp::Pipeline linePipe_;
	vpp::Pipeline pointPipe_;
	vpp::PipelineLayout pipeLayout_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::SubBuffer flatVerts_;
	vpp::SubBuffer pointVerts_;
	vpp::SubBuffer cameraUbo_;
};

int main(int argc, const char** argv) {
	BezierApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

