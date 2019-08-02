#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/geometry.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>

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
	// volume of the triangle times 2
	float vt2 = tkn::cross(tri.b - tri.a, tri.c - tri.a);

	// n1x means dN1(x, y)/dx, where N1 is the first of the
	// basis functions. In this case (linear basis functions over triangle)
	// N1 would be the first barycentric coordinate (i.e. the factor
	// for tri.a) of x,y
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
	float volume = 0.5 * tkn::cross(tri.b - tri.a, tri.c - tri.a);
	auto b = B(tri);
	return volume * nytl::transpose(b) * D * b;
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
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		((void) cb);
	}

	void update(double dt) override {
		App::update(dt);
	}

	const char* name() const override { return "SoftBody (FEM)"; }
};

int main(int argc, const char** argv) {
	SoftBodyApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

