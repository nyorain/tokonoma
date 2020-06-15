#include <tkn/singlePassApp.hpp>
#include <tkn/bits.hpp>
#include <tkn/levelView.hpp>
#include <tkn/transform.hpp>
#include <tkn/render.hpp>
#include <ny/mouseButton.hpp>
#include <nytl/approxVec.hpp>
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
		// TODO: shouldn't this be a matrix?
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

float sign(float x) {
	return (x > 0) ? 1.f : (x < 0) ? -1.f : 0.f;
}

// Singular value decomposition of a 2x2 matrix
struct SVD {
	nytl::Mat2f u, sig, v;
};

// mainly from https://sites.ualberta.ca/~mlipsett/ENGM541/Readings/svd_ellis.pdf
SVD svd(const nytl::Mat2f m) {
	SVD ret;
	auto tm = transpose(m);

	// find u
	auto m1 = m * tm;
	auto phi = 0.5f * std::atan2(m1[0][1] + m1[1][0], m1[0][0] - m1[1][1]);
	auto cphi = std::cos(phi);
	auto sphi = std::sin(phi);
	ret.u = {cphi, -sphi, sphi, cphi};

	// find singular values
	auto sum = m1[0][0] + m1[1][1];
	auto dif = m1[0][0] - m1[1][1];
	dif = std::sqrt(dif * dif + 4 * m1[0][1] * m1[1][0]);
	ret.sig = {
		std::sqrt(0.5f * (sum + dif)), 0,
		0, std::sqrt(0.5f * (sum - dif))
	};

	auto m2 = tm * m;
	auto theta = 0.5f * std::atan2(m2[0][1] + m2[1][0], m2[0][0] - m2[1][1]);
	auto ctheta = std::cos(theta);
	auto stheta = std::sin(theta);
	auto w = nytl::Mat2f {ctheta, -stheta, stheta, ctheta};

	auto s = transpose(ret.u) * m * w;
	auto c = nytl::Mat2f {sign(s[0][0]), 0, 0, sign(s[1][1])};
	ret.v = w * c;

	return ret;
}

float det(nytl::Mat2f m) {
	return m[0][0] * m[1][1] - m[0][1] * m[1][0];
}

nytl::Mat2f inverse(nytl::Mat2f m) {
	auto d = det(m);
	return (1 / d) * nytl::Mat2f {
		m[1][1], -m[0][1],
		-m[1][0], m[0][0],
	};
}

nytl::Mat2f F(const Triangle& tri,
		nytl::Vec2f u1, nytl::Vec2f u2, nytl::Vec2f u3) {
	float vt2 = nytl::cross(tri.b - tri.a, tri.c - tri.a);
	float n1x = (tri.b.y - tri.c.y) / vt2;
	float n1y = (tri.c.x - tri.b.x) / vt2;
	float n2x = (tri.c.y - tri.a.y) / vt2;
	float n2y = (tri.a.x - tri.c.x) / vt2;
	float n3x = (tri.a.y - tri.b.y) / vt2;
	float n3y = (tri.b.x - tri.a.x) / vt2;

	// TODO: not sure about 1+
	// should be right though since F = E + grad(u), right?
	// and grad(u) = sum(grad(N_i) * u_i)
	return {
		1 + n1x * u1.x + n2x * u2.x + n3x * u3.x,
		n1y * u1.x + n2y * u2.x + n3y * u3.x,
		n1x * u1.y + n2x * u2.y + n3x * u3.y,
		1 + n1y * u1.y + n2y * u2.y + n3y * u3.y,
	};
}

// Using simple euler integration
void step(Body& fe, float dt) {
	// dlg_info(" ============= STEP ================ ");

	// integrate velocity
	for(auto& point : fe.points) {
		point.u += dt * point.udot;
	}

	// integrate acceleration (i.e. apply forces)
	for(auto& tri : fe.elems) {
		auto& a = fe.points[tri.a];
		auto& b = fe.points[tri.b];
		auto& c = fe.points[tri.c];

		auto f = F({a.pos, b.pos, c.pos}, a.u, b.u, c.u);
		auto [u, sig, v] = svd(f);
		auto vt = transpose(v);
		dlg_assertm(u * sig * vt == nytl::approx(f, 0.01), "{} \nvs\n {}",
			u * sig * vt, f);

		auto rot = u * vt;
		auto r = nytl::Mat<6, 6, float> {
			rot[0][0], rot[0][1], 0, 0, 0, 0,
			rot[1][0], rot[1][1], 0, 0, 0, 0,
			0, 0, rot[0][0], rot[0][1], 0, 0,
			0, 0, rot[1][0], rot[1][1], 0, 0,
			0, 0, 0, 0, rot[0][0], rot[0][1],
			0, 0, 0, 0, rot[1][0], rot[1][1],
		};

		// dlg_info("rot: {}", rot);
		// dlg_info("sig: {}", sig);
		// auto K = r * tri.K * transpose(r);
		// auto K = tri.K;

		nytl::Vec<6, float> x = {
			a.pos.x, a.pos.y,
			b.pos.x, b.pos.y,
			c.pos.x, c.pos.y,
		};
		nytl::Vec<6, float> uv = {
			a.u.x, a.u.y,
			b.u.x, b.u.y,
			c.u.x, c.u.y,
		};

		// nytl::Vec<6, float> f0 = r * tri.K * x;
		// nytl::Vec<6, float> fcorot = -r * tri.K * transpose(r) * (x + uv);
		// auto force = fcorot + f0;
		auto force = -r * tri.K * (transpose(r) * (x + uv) - x);

		// basic friction
		force -= 0.01 * nytl::Vec<6, float>{
			a.udot.x, a.udot.y,
			b.udot.x, b.udot.y,
			c.udot.x, c.udot.y,
		};

		auto dud = dt * tri.invm * force;
		a.udot.x += dud[0];
		a.udot.y += dud[1];
		b.udot.x += dud[2];
		b.udot.y += dud[3];
		c.udot.x += dud[4];
		c.udot.y += dud[5];
	}
}

class SoftBodyApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	static constexpr auto width = 32u;
	static constexpr auto height = 3u;
	static constexpr float scale = 0.25;
	static constexpr float E = 500.0;
	static constexpr float v = 0.3;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
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
		const auto d = D(E, v);
		for(auto& tri : body_.elems) {
			auto& a = body_.points[tri.a];
			auto& b = body_.points[tri.b];
			auto& c = body_.points[tri.c];
			auto area = 0.5 * nytl::cross(b.pos - a.pos, c.pos - a.pos);
			tri.invm = 1 / (0.25 * density * area);
			tri.K = K(Triangle{a.pos, b.pos, c.pos}, d);
		}

		// create pipeline
		auto& dev = vkDevice();

		auto bindings = std::array {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex)
		};

		dsLayout_.init(dev, bindings);
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

	void step(double dt) {
		::step(body_, dt);

		// mouse interaction
		if(mouseDown_) {
			for(auto i = 0u; i < height; ++i) {
				auto id = i * width;
				auto& p = body_.points[id];
				auto d = levelAttractor_ - (p.pos + p.u);
				body_.points[id].udot += 100 * dt * d;
			}

			// body_.points[0].u = levelAttractor_ - body_.points[0].pos;
			// body_.points[0].udot = {0.f, 0.f};
		}

		// gravity interaction
		for(auto& p : body_.points) {
			p.udot.y -= dt * 9.81;
		}

		// fix center points
		for(auto i = 0u; i < height; ++i) {
			auto id = i * width + width / 2;
			body_.points[id].u = {0.f, 0.f};
			body_.points[id].udot = {0.f, 0.f};
		}
	}

	void update(double dt) override {
		App::update(dt);
		// TODO: accum system and match dt
		for(auto i = 0u; i < 10; ++i) {
			step(0.002);
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

	void resize(unsigned w, unsigned h) override {
		App::resize(w, h);
		view_.size = tkn::levelViewSize(w / float(h), 10.f);
		updateView_ = true;
	}

	bool mouseButton(const swa_mouse_button_event& ev) override {
		if(Base::mouseButton(ev)) {
			return true;
		}

		if(ev.button != swa_mouse_button_left) {
			return false;
		}

		mouseDown_ = ev.pressed;
		levelAttractor_ = tkn::windowToLevel(windowSize(), view_, {ev.x, ev.y});
		return true;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		levelAttractor_ = tkn::windowToLevel(windowSize(), view_, {ev.x, ev.y});
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
	return tkn::appMain<SoftBodyApp>(argc, argv);
}

