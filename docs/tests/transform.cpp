#include <tkn/quaternion.hpp>
#include <tkn/types.hpp>
#include <tkn/transform.hpp>
#include <nytl/approx.hpp>
#include <nytl/approxVec.hpp>
#include <nytl/matOps.hpp>
#include <dlg/dlg.hpp>
#include "bugged.hpp"
#include <random>

// TODO:
// - independent tkn::base tests
// - orientMat for 3D
// - other transform matrices: scaleMat, translateMat

using namespace tkn;
using namespace tkn::types;
using nytl::approx;
using namespace nytl::approxOps;
using namespace nytl::constants;

std::mt19937 rgen = []{
	std::mt19937 r;
	r.seed(0);
	return r;
}();

auto eps = 0.001;
TEST(lookAt) {
	// quaternion overload
	auto q = Quaternion {};
	auto pos = Vec3f {1.f, 1.f, 1.f};

	auto mat = lookAt(q, pos);
	EXPECT(multPos(mat, pos), approx(Vec3f{0.f, 0.f, 0.f}));
	EXPECT(multPos(mat, Vec3f {2.f, 1.f, 1.f}), approx(Vec3f{1.f, 0.f, 0.f}));
	EXPECT(multPos(mat, Vec3f {1.f, 1.f, 2.f}), approx(Vec3f{0.f, 0.f, 1.f}));
	EXPECT(multPos(mat, Vec3f {0.f, 0.f, 0.f}), approx(Vec3f{-1.f, -1.f, -1.f}));

	pos = Vec3f {0.f, 0.f, 0.f};
	q = normalized(Quaternion::axisAngle(1.f, 2.f, 3.f, 1.23456));
	EXPECT(transpose(lookAt(q, pos)), approx(toMat<4, float>(q), eps));

	// generate a random orthonormal base, random position and test
	// that lookAt correctly transforms the random pos offset by the
	// axis vector to the unit vectors.
	auto num = 100u;
	std::uniform_real_distribution<float> d(-1.f, 1.f);
	for(auto i = 0u; i < num; ++i) {
		auto b0 = normalized(nytl::Vec3f{d(rgen), d(rgen), d(rgen)});
		auto [b1, b2] = tkn::base(b0);
		EXPECT(dot(b0, b1), approx(0.f, eps));
		EXPECT(dot(b0, b2), approx(0.f, eps));
		EXPECT(dot(b1, b2), approx(0.f, eps));

		auto m = nytl::Mat3d(nytl::Mat3f{b0, b1, b2});
		q = Quaternion::fromMat(transpose(m));
		auto pos = nytl::Vec3f{d(rgen), d(rgen), d(rgen)};

		auto l = tkn::lookAt(q, pos);
		EXPECT(multPos(l, pos), approx(Vec3f{0.f, 0.f, 0.f}));
		EXPECT(multPos(l, pos + b0), approx(Vec3f{1.f, 0.f, 0.f}, eps));
		EXPECT(multPos(l, pos + b1), approx(Vec3f{0.f, 1.f, 0.f}, eps));
		EXPECT(multPos(l, pos + b2), approx(Vec3f{0.f, 0.f, 1.f}, eps));

		// check it's the same as the other overload
		auto z = apply(q, Vec3f{0.f, 0.f, 1.f});
		auto y = apply(q, Vec3f{0.f, 1.f, 0.f});
		EXPECT(l, approx(lookAt(pos, z, y), eps));
	}

	// Non-quaternion version
	pos = Vec3f{0.f, 6.f, -23.234};
	auto dir = Vec3f{0.f, 0.f, 1.f};
	auto l = tkn::lookAt(pos, dir, Vec3f{0.f, 1.f, 0.f});
	EXPECT(multPos(l, pos), approx(Vec3f{0.f, 0.f, 0.f}, eps));
	EXPECT(multPos(l, pos + 2.f * Vec3f{0.f, 0.f, 1.f}), approx(Vec3f{0.f, 0.f, 2.f}, eps));
	EXPECT(multPos(l, pos + 1.f * Vec3f{1.f, 0.f, 0.f}), approx(Vec3f{1.f, 0.f, 0.0f}, eps));
	EXPECT(multPos(l, pos - 5.3f * Vec3f{0.f, 1.f, 0.f}), approx(Vec3f{0.f, -5.3f, 0.0f}, eps));

	dir = normalized(Vec3f{3.f, -0.75f, 0.3333f});
	l = tkn::lookAt(pos, dir, Vec3f{0.f, 1.f, 0.f});
	EXPECT(multPos(l, pos), approx(Vec3f{0.f, 0.f, 0.f}, eps));
	EXPECT(multPos(l, pos + 0.1f * dir), approx(Vec3f{0.f, 0.f, 0.1f}, eps));
	EXPECT(multPos(l, pos - 10.f * dir), approx(Vec3f{0.f, 0.f, -10.f}, eps));
}

TEST(perspective) {
	auto fov = float(0.5 * nytl::constants::pi); // 90 degrees, i.e. diagonal
	auto mat = perspective(fov, 1.f, -1.f, -10.f); // negative z, RH

	EXPECT(multPos(mat, Vec3f {0.f, 0.f, -1.f}), approx(Vec3f {0.f, 0.f, 0.f}, eps));
	EXPECT(multPos(mat, Vec3f {0.f, 1.f, -1.f}), approx(Vec3f {0.f, 1.f, 0.f}, eps));
	EXPECT(multPos(mat, Vec3f {10.f, 10.f, -10.f}), approx(Vec3f {1.f, 1.f, 1.f}, eps));

	mat = perspective(fov, 1.f, 1.f, 10.f); // positive z, LH
	EXPECT(multPos(mat, Vec3f {0.f, 0.f, 1.f}), approx(Vec3f {0.f, 0.f, 0.f}, eps));
	EXPECT(multPos(mat, Vec3f {0.f, 1.f, 1.f}), approx(Vec3f {0.f, 1.f, 0.f}, eps));
	EXPECT(multPos(mat, Vec3f {10.f, 1.f, 10.f}), approx(Vec3f {1.f, 0.1f, 1.f}, eps));

	mat = perspective(fov, 2.f, 0.1f, 10.f); // using 'aspect'
	EXPECT(multPos(mat, Vec3f {0.f, 0.f, 0.1f}), approx(Vec3f {0.f, 0.f, 0.f}, eps));
	EXPECT(multPos(mat, Vec3f {0.f, 0.1f, 0.1f}), approx(Vec3f {0.f, 1.f, 0.f}, eps));
	EXPECT(multPos(mat, Vec3f {10.f, 1.f, 10.f}), approx(Vec3f {0.5f, 0.1f, 1.f}, eps));

	mat = perspective(fov, 1.f, -10.f, -1.f); // RH, reversed depth
	EXPECT(multPos(mat, Vec3f {0.f, 0.f, -1.f}), approx(Vec3f {0.f, 0.f, 1.f}, eps));
	EXPECT(multPos(mat, Vec3f {0.f, 1.f, -1.f}), approx(Vec3f {0.f, 1.f, 1.f}, eps));
	EXPECT(multPos(mat, Vec3f {10.f, 10.f, -10.f}), approx(Vec3f {1.f, 1.f, 0.f}, eps));
	EXPECT(mat, approx(perspectiveRev(fov, 1.f, -1.f, -10.f)));

	mat = perspectiveRevInf(fov, 1.f, 0.01f);
	EXPECT(multPos(mat, Vec3f {0.f, 0.f, 0.01f}), approx(Vec3f {0.f, 0.f, 1.f}, eps));
	EXPECT(multPos(mat, Vec3f {0.f, 0.01f, 0.01f}), approx(Vec3f {0.f, 1.f, 1.f}, eps));
	EXPECT(multPos(mat, Vec3f {1000.f, -1000.f, 10000.f}),
		approx(Vec3f {0.1f, -0.1f, float(eps)}, eps));

	mat = perspectiveRevInf(fov, 1.f, -1.f);
	EXPECT(multPos(mat, Vec3f {0.f, 0.f, -1.f}), approx(Vec3f {0.f, 0.f, 1.f}, eps));
	EXPECT(multPos(mat, Vec3f {1.f, 1.f, -1.f}), approx(Vec3f {1.f, 1.f, 1.f}, eps));
	EXPECT(multPos(mat, Vec3f {0.f, 0.f, -100000.f}),
		approx(Vec3f {0.f, 0.f, float(eps)}, eps));
}

TEST(rotateVec) {
	auto axis = nytl::Vec3f{0.f, 0.f, 1.f};
	auto angle = float(0.5 * nytl::constants::pi);
	EXPECT(rotate(Vec3f{1.f, 0.f, 0.f}, axis, angle),
		approx(Vec3f{0.f, 1.f, 0.f}, eps));
	EXPECT(rotate(Vec3f{0.f, 2.f, 0.f}, axis, angle),
		approx(Vec3f{-2.f, 0.f, 0.f}, eps));
	EXPECT(rotate(Vec3f{0.f, 0.f, 0.f}, axis, angle),
		approx(Vec3f{0.f, 0.f, 0.f}, eps));
	EXPECT(rotate(Vec3f{0.f, 0.f, -6.f}, axis, angle),
		approx(Vec3f{0.f, 0.f, -6.f}, eps));
}

TEST(orient2) {
	auto m = orientMat<2>(nytl::Vec2f{1.f, 0.f}, nytl::Vec2f{1.f, 1.f});
	EXPECT((m * nytl::Vec2f{1.f, 0.f}), approx(Vec2f{1.f, 1.f}));
	EXPECT((m * nytl::Vec2f{2.f, 0.f}), approx(Vec2f{2.f, 2.f}));
	EXPECT((m * nytl::Vec2f{0.f, 1.f}), approx(Vec2f{-1.f, 1.f}));
	EXPECT((m * nytl::Vec2f{0.f, 0.f}), approx(Vec2f{0.f, 0.f}));

	EXPECT(approx(m), orientMat<2>(nytl::Vec2f{0.f, 1.f}, nytl::Vec2f{-1.f, 1.f}));
	EXPECT(
		approx(orientMat<2>(nytl::Vec2f{0.f, 1.f}, nytl::Vec2f{0.f, 2.f})),
		orientMat<2>(nytl::Vec2f{1.f, 0.f}, nytl::Vec2f{2.f, 0.f}));
	EXPECT(
		approx(orientMat<2>(nytl::Vec2f{0.f, 1.f}, nytl::Vec2f{1.f, 0.f})),
		rotateMat<2>(-0.5 * pi));
}

TEST(orient3) {
	auto m = orientMat<3>(nytl::Vec3f{1.f, 0.f, 0.f}, nytl::Vec3f{1.f, 0.f, 0.f});
	EXPECT(m, approx(nytl::identity<3, float>(), eps));

	m = orientMat<3>(nytl::Vec3f{1.f, 0.f, 0.f}, nytl::Vec3f{0.f, 1.f, 0.f});
	EXPECT((m * Vec3f{1.f, 0.f, 0.f}), approx(Vec3f{0.f, 1.f, 0.f}, eps));
	EXPECT((m * Vec3f{0.f, 1.f, 0.f}), approx(Vec3f{-1.f, 0.f, 0.f}, eps));
	EXPECT((m * Vec3f{0.f, 0.f, 1.f}), approx(Vec3f{0.f, 0.f, 1.f}, eps));

	auto isqrt2 = 1.f / std::sqrt(2.f);
	m = orientMat<3>(nytl::Vec3f{1.f, 0.f, 0.f}, nytl::Vec3f{isqrt2, isqrt2, 0.f});
	EXPECT((m * Vec3f{1.f, 0.f, 0.f}), approx(Vec3f{isqrt2, isqrt2, 0.f}, eps));
	EXPECT((m * Vec3f{0.f, 1.f, 0.f}), approx(Vec3f{-isqrt2, isqrt2, 0.f}, eps));
	EXPECT((m * Vec3f{0.f, 0.f, 1.f}), approx(Vec3f{0.f, 0.f, 1.f}, eps));
}

TEST(frustumPos) {
	auto m = frustum(-10.f, 10.f, 0.f, 5.f, 0.1f, 20.f);

	EXPECT(multPos(m, Vec3f{0.f, 2.5f, 0.1f}), approx(Vec3f{0.f, 0.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{-10.f, 0.f, 0.1f}), approx(Vec3f{-1.f, -1.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{10.f, 5.f, 0.1f}), approx(Vec3f{1.f, 1.f, 0.f}, eps));

	EXPECT(multPos(m, Vec3f{10.f, 2.5f, 0.1f}), approx(Vec3f{1.f, 0.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{-2000.f, 500.f, 20.f}), approx(Vec3f{-1.f, 0.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{2000.f, 500.f, 20.f}), approx(Vec3f{1.f, 0.f, 1.f}, eps));

	EXPECT(multPos(m, Vec3f{0.f, 0.f, 0.1f}), approx(Vec3f{0.f, -1.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{0.f, 5.f, 0.1f}), approx(Vec3f{0.f, 1.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{0.f, 0.f, 20.f}), approx(Vec3f{0.f, -1.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{0.f, 1000.f, 20.f}), approx(Vec3f{0.f, 1.f, 1.f}, eps));
}

TEST(frustumNeg) {
	auto m = frustum(0.f, 1.f, -1.f, 1.f, -0.5f, -1000.f);

	EXPECT(multPos(m, Vec3f{0.5f, 0.f, -0.5f}), approx(Vec3f{0.f, 0.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{0.f, -1.f, -0.5f}), approx(Vec3f{-1.f, -1.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{1.f, 1.f, -0.5f}), approx(Vec3f{1.f, 1.f, 0.f}, eps));

	EXPECT(multPos(m, Vec3f{0.f, -2000.f, -1000.f}), approx(Vec3f{-1.f, -1.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{2000.f, 2000.f, -1000.f}), approx(Vec3f{1.f, 1.f, 1.f}, eps));
}

TEST(frustumRevPos) {
	auto m = frustumRev(0.f, 1.f, -1.f, 1.f, 0.1f, 10.f);
	EXPECT(multPos(m, Vec3f{0.5f, 0.f, 0.1f}), approx(Vec3f{0.f, 0.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{0.f, -1.f, 0.1f}), approx(Vec3f{-1.f, -1.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{1.f, 1.f, 0.1f}), approx(Vec3f{1.f, 1.f, 1.f}, eps));

	EXPECT(multPos(m, Vec3f{0.f, 100.f, 10.f}), approx(Vec3f{-1.f, 1.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{100.f, -100.f, 10.f}), approx(Vec3f{1.f, -1.f, 0.f}, eps));
}

TEST(frustumRevNeg) {
	auto m = frustumRev(0.f, 1.f, -1.f, 1.f, -0.1f, -10.f);
	EXPECT(multPos(m, Vec3f{0.5f, 0.f, -0.1f}), approx(Vec3f{0.f, 0.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{0.f, -1.f, -0.1f}), approx(Vec3f{-1.f, -1.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{1.f, 1.f, -0.1f}), approx(Vec3f{1.f, 1.f, 1.f}, eps));

	EXPECT(multPos(m, Vec3f{0.f, 100.f, -10.f}), approx(Vec3f{-1.f, 1.f, 0.f}, eps));
	EXPECT(multPos(m, Vec3f{100.f, -100.f, -10.f}), approx(Vec3f{1.f, -1.f, 0.f}, eps));
}

TEST(frustumRevInfPos) {
	auto m = frustumRevInf(0.f, 1.f, -1.f, 1.f, 0.1f);
	EXPECT(multPos(m, Vec3f{0.5f, 0.f, 0.1f}), approx(Vec3f{0.f, 0.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{0.f, -1.f, 0.1f}), approx(Vec3f{-1.f, -1.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{1.f, 1.f, 0.1f}), approx(Vec3f{1.f, 1.f, 1.f}, eps));

	EXPECT(multPos(m, Vec3f{0.f, 10000.f, 1000.f}),
		approx(Vec3f{-1.f, 1.f, 0.01}, 0.01));
}

TEST(frustumRevInfNeg) {
	auto m = frustumRevInf(0.f, 1.f, -1.f, 1.f, -0.1f);
	EXPECT(multPos(m, Vec3f{0.5f, 0.f, -0.1f}), approx(Vec3f{0.f, 0.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{0.f, -1.f, -0.1f}), approx(Vec3f{-1.f, -1.f, 1.f}, eps));
	EXPECT(multPos(m, Vec3f{1.f, 1.f, -0.1f}), approx(Vec3f{1.f, 1.f, 1.f}, eps));

	EXPECT(multPos(m, Vec3f{0.f, -10000.f, -1000.f}),
		approx(Vec3f{-1.f, -1.f, 0.01}, 0.01));
}

TEST(ortho) {
	auto m1 = ortho(-1.f, 1.f, 0.f, 1.f, 0.f, 1.f); // positive near/far
	EXPECT(multPos(m1, Vec3f{0.f, 0.5f, 0.f}), approx(Vec3f{0.f, 0.f, 0.f}));
	EXPECT(multPos(m1, Vec3f{0.f, 0.5f, 0.5f}), approx(Vec3f{0.f, 0.f, 0.5f}));
	EXPECT(multPos(m1, Vec3f{0.f, 0.5f, 0.1f}), approx(Vec3f{0.f, 0.f, 0.1f}));
	EXPECT(multPos(m1, Vec3f{-1.f, 0.f, 0.f}), approx(Vec3f{-1.f, -1.f, 0.f}));
	EXPECT(multPos(m1, Vec3f{1.f, 1.f, 0.f}), approx(Vec3f{1.f, 1.f, 0.f}));
	EXPECT(multPos(m1, Vec3f{1.f, 1.f, 1.f}), approx(Vec3f{1.f, 1.f, 1.f}));

	auto m2 = ortho(-1.f, 1.f, 0.f, 1.f, -1.f, -1000.f); // negative near/far
	EXPECT(multPos(m2, Vec3f{-1.f, 0.f, -1.f}), approx(Vec3f{-1.f, -1.f, 0.f}, eps));
	EXPECT(multPos(m2, Vec3f{1.f, 1.f, -1.f}), approx(Vec3f{1.f, 1.f, 0.f}, eps));
	EXPECT(multPos(m2, Vec3f{1.f, 1.f, -1000.f}), approx(Vec3f{1.f, 1.f, 1.f}, eps));
	EXPECT(multPos(m2, Vec3f{0.5f, 0.25f, -500.f}), approx(Vec3f{0.5f, -0.5f, 0.5f}, eps));

	auto m3 = ortho(-1.f, 1.f, 0.f, 100.f, 1.f, 0.f); // reversed depth
	EXPECT(multPos(m3, Vec3f{0.f, 50.f, 0.f}), approx(Vec3f{0.f, 0.f, 1.f}));
	EXPECT(multPos(m3, Vec3f{0.f, 50.f, 0.5f}), approx(Vec3f{0.f, 0.f, 0.5f}));
	EXPECT(multPos(m3, Vec3f{-1.f, 0.f, 0.f}), approx(Vec3f{-1.f, -1.f, 1.f}));
	EXPECT(multPos(m3, Vec3f{1.f, 100.f, 0.f}), approx(Vec3f{1.f, 1.f, 1.f}));
	EXPECT(multPos(m3, Vec3f{1.f, 100.f, 1.f}), approx(Vec3f{1.f, 1.f, 0.f}));
	EXPECT(multPos(m3, Vec3f{-1.f, 100.f, 1.f}), approx(Vec3f{-1.f, 1.f, 0.f}));

	auto m4 = ortho(-1.f, 1.f, 0.f, 1.f, -10.f, 10.f); // signs of near/far irrelevant
	EXPECT(multPos(m4, Vec3f{0.f, 0.5f, -10.f}), approx(Vec3f{0.f, 0.f, 0.f}));
	EXPECT(multPos(m4, Vec3f{0.f, 0.5f, 0.f}), approx(Vec3f{0.f, 0.f, 0.5f}));
	EXPECT(multPos(m4, Vec3f{-1.f, 0.f, 5.f}), approx(Vec3f{-1.f, -1.f, 0.75f}));
	EXPECT(multPos(m4, Vec3f{1.f, 1.f, -10.f}), approx(Vec3f{1.f, 1.f, 0.f}));
	EXPECT(multPos(m4, Vec3f{1.f, 1.f, 10.f}), approx(Vec3f{1.f, 1.f, 1.f}));
	EXPECT(multPos(m4, Vec3f{1.f, 0.f, 10.f}), approx(Vec3f{1.f, -1.f, 1.f}));
}
