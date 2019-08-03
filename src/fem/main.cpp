#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/bits.hpp>
#include <tkn/transform.hpp>
#include <tkn/render.hpp>
#include <ny/mouseButton.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>
#include <vpp/queue.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/trackedDescriptor.hpp>

#include <shaders/fem.body.vert.h>
#include <shaders/fem.body.frag.h>

struct Triangle {
	nytl::Vec2f a;
	nytl::Vec2f b;
	nytl::Vec2f c;
};

struct Body {
	struct Point {
		nytl::Vec2f pos; // original position
		nytl::Vec2f u {}; // current position - original position
		nytl::Vec2f udot {}; // rate of change of current position (velocity-like)
	};

	struct FiniteElem {
		unsigned a, b, c; // indices into points; form triangle
		nytl::Mat<6, 6, float> K; // precomputed stiffness matrix
		float invm; // inverse mass (inverse lumped mass matrix diagonal entry)
	};

	std::vector<Point> points;
	std::vector<FiniteElem> elems;
};

// Elasticity matrix, relation between strain and tension
// E: youngs-modul: relation strain and tension, can be compared
//   to spring constant in mass-spring-systems [0, inf]?
// v: (like greek ny) poisson's ratio [0, 1]
//   basically determines if and how the body conserves
//   mass during stretching? Should not be 0.5
nytl::Mat3f D(float E, float v) {
	auto a = 1 - v;
	auto b = 0.5f * (1 - 2 * v);
	auto fac = E / ((1 + v) * (1 - 2 * v));
	return fac * nytl::Mat3f{
		a, v, 0,
		v, a, 0,
		0, 0, b,
	};
}

// Using (some kind of) voigt notation
nytl::Mat<3, 6, float> B(const Triangle& tri) {
	// area of the triangle times 2
	float vt2 = nytl::cross(tri.b - tri.a, tri.c - tri.a);

	// n1x means dN1(x, y)/dx, where N1 is the first of the
	// basis functions. In this case (linear basis functions over triangle)
	// N1 would be the first barycentric coordinate (i.e. the factor
	// for tri.a) of x,y. Can be computed like this because barycentric
	// coords (as function) are linear, i.e. the derivate constant
	float n1x = (tri.b.y - tri.c.y) / vt2;
	float n1y = (tri.c.x - tri.b.x) / vt2;
	float n2x = (tri.c.y - tri.a.y) / vt2;
	float n2y = (tri.a.x - tri.c.x) / vt2;
	float n3x = (tri.a.y - tri.b.y) / vt2;
	float n3y = (tri.b.x - tri.a.x) / vt2;

	return nytl::Mat<3, 6, float> {
		n1x, 0,   n2x, 0,   n3x, 0,
		0,   n1y, 0,   n2y, 0,   n3y,
		n1y, n1x, n2y, n2x, n3y, n3x,
	};
}

nytl::Mat<6, 6, float> K(const Triangle& tri, const nytl::Mat3f& D) {
	float area = 0.5 * nytl::cross(tri.b - tri.a, tri.c - tri.a);
	auto b = B(tri);
	return area * nytl::transpose(b) * D * b;
}

// Using simple euler integration
void step(Body& fe, float dt) {
	// integrate velocity
	for(auto& point : fe.points) {
		point.u += dt * point.udot;
	}

	// integrate acceleration (i.e. apply forces)
	for(auto& tri : fe.elems) {
		auto& a = fe.points[tri.a];
		auto& b = fe.points[tri.b];
		auto& c = fe.points[tri.c];

		nytl::Vec<6, float> u = {
			a.u.x, a.u.y,
			b.u.x, b.u.y,
			c.u.x, c.u.y,
		};

		auto dud = -dt * tri.invm * tri.K * u; // delta u dot
		a.udot.x += dud[0];
		a.udot.y += dud[1];
		b.udot.x += dud[2];
		b.udot.y += dud[3];
		c.udot.x += dud[4];
		c.udot.y += dud[5];
	}
}

class SoftBodyApp : public tkn::App {
public:
	static constexpr auto width = 50u;
	static constexpr auto height = 5u;
	const float scale = 0.1;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		// init body
		for(auto y = 0u; y < height; ++y) {
			for(auto x = 0u; x < width; ++x) {
				if(x != width - 1 && y != height - 1) {
					auto& tri1 = body_.elems.emplace_back();
					tri1.a = body_.points.size();
					tri1.b = body_.points.size() + 1;
					tri1.c = body_.points.size() + width + 1;

					auto& tri2 = body_.elems.emplace_back();
					tri2.a = body_.points.size();
					tri2.b = body_.points.size() + width + 1;
					tri2.c = body_.points.size() + width;
				}

				auto& point = body_.points.emplace_back();
				point.pos = {scale * x, scale * y};
				point.u = point.udot = {0.f, 0.f};
			}
		}

		view_.center = 0.5f * scale * nytl::Vec2f{width, height};

		// pre-compute fe properties
		const float density = 1.f;
		const auto d = D(0.1, 0.1);
		for(auto& tri : body_.elems) {
			auto& a = body_.points[tri.a];
			auto& b = body_.points[tri.b];
			auto& c = body_.points[tri.c];
			auto area = 0.5 * nytl::cross(b.pos - a.pos, c.pos - a.pos);
			tri.invm = 1 / (0.25 * density * area);
			tri.K = K(Triangle{a.pos, b.pos, c.pos}, d);
		}

		// create pipeline
		auto& dev = vulkanDevice();

		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex)
		};

		dsLayout_ = {dev, bindings};
		pipeLayout_ = {dev, {{dsLayout_}}, {}};

		vpp::ShaderModule vertShader{dev, fem_body_vert_data};
		vpp::ShaderModule fragShader{dev, fem_body_frag_data};
		vpp::GraphicsPipelineInfo gpi {renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples()};

		vk::VertexInputAttributeDescription attribs[1];
		attribs[0].format = vk::Format::r32g32Sfloat;

		vk::VertexInputBindingDescription bufs[1];
		bufs[0].inputRate = vk::VertexInputRate::vertex;
		bufs[0].stride = sizeof(nytl::Vec2f);

		gpi.vertex.pVertexAttributeDescriptions = attribs;
		gpi.vertex.vertexAttributeDescriptionCount = 1;
		gpi.vertex.pVertexBindingDescriptions = bufs;
		gpi.vertex.vertexBindingDescriptionCount = 1;

		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		gpi.rasterization.polygonMode = vk::PolygonMode::fill;
		gpi.depthStencil.depthTestEnable = false;

		pipe_ = {dev, gpi.info()};

		// create buffers
		vertices_ = {dev.bufferAllocator(),
			sizeof(nytl::Vec2f) * body_.points.size(),
			vk::BufferUsageBits::vertexBuffer, dev.hostMemoryTypes()};
		indices_ = {dev.bufferAllocator(),
			sizeof(std::uint32_t) * 3 * body_.elems.size(),
			vk::BufferUsageBits::indexBuffer | vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};
		ubo_ = {dev.bufferAllocator(), sizeof(nytl::Mat4f),
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		// upload data
		// vertices will be uploaded in updateDevice (every frame anyways)
		// indices
		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		std::vector<std::uint32_t> indices;
		for(auto& tri : body_.elems) {
			indices.push_back(tri.a);
			indices.push_back(tri.b);
			indices.push_back(tri.c);
		}

		indexCount_ = indices.size();
		auto stage = vpp::fillStaging(cb, indices_, tkn::bytes(indices));

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// ds
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{ubo_}});

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindVertexBuffers(cb, 0, {{vertices_.buffer()}},
			{{vertices_.offset()}});
		vk::cmdBindIndexBuffer(cb, indices_.buffer(), indices_.offset(),
			vk::IndexType::uint32);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		vk::cmdDrawIndexed(cb, indexCount_, 1, 0, 0, 0);
	}

	void update(double dt) override {
		App::update(dt);

		step(body_, dt);
		if(mouseDown_) {
			for(auto i = 0u; i < height; ++i) {
				auto id = i * width;
				auto& p = body_.points[id];
				auto d = levelAttractor_ - (p.pos + p.u);
				body_.points[id].udot += dt * d;
			}
		}

		App::scheduleRedraw();
	}

	void updateDevice() override {
		{
			std::vector<nytl::Vec2f> vertices;
			for(auto& p : body_.points) {
				vertices.push_back(p.pos + p.u);
			}

			auto map = vertices_.memoryMap();
			auto span = map.span();
			tkn::write(span, tkn::bytes(vertices));
			map.flush();
		}

		if(updateView_) {
			updateView_ = false;
			auto map = ubo_.memoryMap();
			auto span = map.span();
			tkn::write(span, levelMatrix(view_));
			map.flush();
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		view_.size = tkn::levelViewSize(ev.size.x / float(ev.size.y), 10.f);
		updateView_ = true;
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button != ny::MouseButton::left) {
			return false;
		}

		mouseDown_ = ev.pressed;
		levelAttractor_ = tkn::windowToLevel(window().size(), view_, ev.position);
		return true;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		levelAttractor_ = tkn::windowToLevel(window().size(), view_, ev.position);
	}

	const char* name() const override { return "SoftBody (FEM)"; }

protected:
	Body body_;
	vpp::SubBuffer vertices_;
	vpp::SubBuffer indices_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::SubBuffer ubo_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	tkn::LevelView view_;
	unsigned indexCount_;
	bool updateView_ {true};
	bool mouseDown_ {false};
	nytl::Vec2f levelAttractor_ {};
};

int main(int argc, const char** argv) {
	SoftBodyApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

