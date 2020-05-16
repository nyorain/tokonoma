#include <tkn/quaternion.hpp>
#include <tkn/types.hpp>
#include <tkn/transform.hpp>
#include <nytl/approx.hpp>
#include <nytl/approxVec.hpp>
#include <nytl/matOps.hpp>
#include <dlg/dlg.hpp>
#include "bugged.hpp"
#include <random>

using namespace tkn;
using namespace tkn::types;
using nytl::approx;
using namespace nytl::approxOps;
using namespace nytl::constants;

std::ostream& print(std::ostream& os, const Quaternion& q) {
	return os << nytl::Vec4d{q.x, q.y, q.z, q.w};
}

std::ostream& operator<<(std::ostream& os, const Quaternion& q) {
	return print(os, q);
}

std::mt19937 rgen = []{
	std::mt19937 r;
	r.seed(0);
	return r;
}();

auto eps = 0.0001;
TEST(apply) {
	auto angle = 0.5 * pi;
	auto q = Quaternion::axisAngle(0.0, 0.0, 1.0, angle);
	auto rotated = apply(q, nytl::Vec3f{1.f, 0.f, 0.f});
	EXPECT(rotated, approx(nytl::Vec3f{0.f, 1.f, 0.f}, eps));

	auto num = 100u;
	std::uniform_real_distribution<double> distr(-0.5 * pi, 0.5 * pi);
	for(auto i = 0u; i < num; ++i) {
		// create random quaternion from euler rotation sequence
		auto a1 = distr(rgen);
		auto a2 = distr(rgen);
		auto a3 = distr(rgen);
		// auto mm = rotateMat<3>({0.f, 0.f, 1.f}, a3) *
		// 	rotateMat<3>({1.f, 0.f, 0.f}, a2) *
		// 	rotateMat<3>({0.f, 1.f, 0.f}, a1);
		auto mm = rotateMat<3>({0.f, 1.f, 0.f}, a1) *
			rotateMat<3>({1.f, 0.f, 0.f}, a2) *
			rotateMat<3>({0.f, 0.f, 1.f}, a3);

		auto q = Quaternion::yxz(a1, a2, a3);

		// 1: test that transforming quaternion back to euler angles
		// gives the same result
		auto angles = eulerAngles(q, RotationSequence::yxz);
		// dlg_info("{} {} {}", a1, a2, a3);
		// dlg_info("{} {} {}", angles[0], angles[1], angles[2]);
		// dlg_info("========");
		EXPECT(a1, approx(angles[0], eps));
		EXPECT(a2, approx(angles[1], eps));
		EXPECT(a3, approx(angles[2], eps));

		auto m = tkn::toMat<3, double>(q);
		auto num = 10u;
		for(auto j = 0u; j < num; ++j) {
			nytl::Vec3d tv{distr(rgen), distr(rgen), distr(rgen)};
			auto qrot = apply(q, tv);

			// 2: Test multiple times that rotating any vector by the
			// quaternion is the same as rotating in by the matrix
			// generated from the quaternion
			EXPECT(m * tv, approx(qrot));

			// 3: Test that manually generating by the rotation sequence
			// without using any quaternion functions at all also
			// gives the same result
			EXPECT(mm * tv, approx(qrot));
		}
	}
}

TEST(toMat) {
	auto num = 100u;
	std::uniform_real_distribution<double> distr(-1.f, 1.f);
	for(auto i = 0u; i < num; ++i) {
		auto q = Quaternion{distr(rgen), distr(rgen), distr(rgen), distr(rgen)};
		q = normalized(q);
		nytl::Mat3f m = {
			apply(q, nytl::Vec3f{1.f, 0.f, 0.f}),
			apply(q, nytl::Vec3f{0.f, 1.f, 0.f}),
			apply(q, nytl::Vec3f{0.f, 0.f, 1.f}),
		};
		EXPECT(toMat<3>(q), approx(transpose(m), eps))
	}
}

TEST(fromMat) {
	auto b1 = nytl::Vec3d{0.f, 1.f, 0.f};
	auto b2 = nytl::Vec3d{0.f, 0.f, -1.f};
	auto b3 = nytl::Vec3d{-1.f, 0.f, 0.f};
	auto m = nytl::Mat3d{b1, b2, b3};
	auto q = Quaternion::fromMat(m);
	EXPECT(norm(q), approx(1.f, eps));
	EXPECT(apply(q, b1), approx(Vec3f{1.f, 0.f, 0.f}, eps));
	EXPECT(apply(q, b2), approx(Vec3f{0.f, 1.f, 0.f}, eps));
	EXPECT(apply(q, b3), approx(Vec3f{0.f, 0.f, 1.f}, eps));
	EXPECT(toMat<3>(q), approx(m, eps));

	q = Quaternion::fromMat(transpose(m));
	EXPECT(norm(q), approx(1.f, eps));
	EXPECT(apply(q, Vec3f{1.f, 0.f, 0.f}), approx(b1, eps));
	EXPECT(apply(q, Vec3f{0.f, 1.f, 0.f}), approx(b2, eps));
	EXPECT(apply(q, Vec3f{0.f, 0.f, 1.f}), approx(b3, eps));
	EXPECT(toMat<3>(q), approx(transpose(m), eps));

	// weird base
	b1 = normalized(nytl::Vec3d{1.323f, -5.234f, 3.666523f});
	b2 = normalized(nytl::Vec3d{-b1.y, b1.x, 0.0});
	b3 = cross(b1, b2);
	m = nytl::Mat3d{b1, b2, b3};
	q = Quaternion::fromMat(transpose(m));
	EXPECT(norm(q), approx(1.f, eps));
	EXPECT(apply(q, Vec3f{1.f, 0.f, 0.f}), approx(b1, eps));
	EXPECT(apply(q, Vec3f{0.f, 1.f, 0.f}), approx(b2, eps));
	EXPECT(apply(q, Vec3f{0.f, 0.f, 1.f}), approx(b3, eps));
	EXPECT((toMat<3, double>(q)), approx(transpose(m), eps));
}

TEST(toEuler) {
	Quaternion q {};
	auto angles = eulerAngles(q, RotationSequence::xyz);
	EXPECT(angles[0], approx(0.0, eps));
	EXPECT(angles[1], approx(0.0, eps));
	EXPECT(angles[2], approx(0.0, eps));

	auto angle = 1.2345;
	q = Quaternion::axisAngle(1.0, 0.0, 0.0, angle);

	auto trivials = {
		RotationSequence::xyx,
		RotationSequence::xzx,
		RotationSequence::yxy,
		RotationSequence::zxz,

		RotationSequence::xyz,
		RotationSequence::xzy,
		RotationSequence::yxz,
		RotationSequence::yzx,
		RotationSequence::zxy,
		RotationSequence::zyx,
	};

	for(auto seq : trivials) {
		auto a = eulerAngles(q, seq);
		unsigned countZero =
			unsigned(a[0] == approx(0.0, eps)) +
			unsigned(a[1] == approx(0.0, eps)) +
			unsigned(a[2] == approx(0.0, eps));
		unsigned countAngle =
			unsigned(a[0] == approx(angle, eps)) +
			unsigned(a[1] == approx(angle, eps)) +
			unsigned(a[2] == approx(angle, eps));
		// dlg_info("{} {} {}", a[0], a[1], a[2]);
		EXPECT(countZero, 2u);
		EXPECT(countAngle, 1u);
	}

}
