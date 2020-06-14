#include <tkn/sh.hpp>
#include <tkn/types.hpp>
#include <tkn/scene/shape.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/approx.hpp>
#include <nytl/approxVec.hpp>
#include <dlg/dlg.hpp>
#include "bugged.hpp"

using namespace tkn::types;
using namespace nytl::approxOps;

// Example of a function that should be representable *exactly*
// in 3rd-order spherical harmonics.
float testFunc(nytl::Vec3d dir) {
	// return 2.4f + dir.x * dir.y - dir.z + 12.54 * dir.z * dir.y * dir.x;
	return 2.4f + 12.3 * dir.x * dir.y - 2 * dir.z + 0.3 * dir.z * dir.x;
}

// Example of a function that cannot be exactly represented.
float testFuncSph(float theta, float phi) {
	return std::cos(4 * theta) + std::sin(phi);
}

nytl::Vec2f hammersley(uint i, uint N) {
	// radical inverse based on
	// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
	uint bits = (i << 16u) | (i >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	float rdi = float(bits) * 2.3283064365386963e-10; // / 0x100000000
	return nytl::Vec2f{float(i) /float(N), rdi};
}

TEST(integrate) {
	using nytl::constants::pi;
	tkn::SH9<float> s {};

	// NOTE: we integrate our sample function over the unit sphere.
	// Integrating using the classic UV sphere grid, i.e.
	// just throwing a grid on phi, theta (as shown below) is a bad
	// idea since it has singularities (at the poles), the samples
	// are not evenly spaced out and along a fixed grid.
	// Better alternatives: monte-carlo integration (low discrepancy)
	// or (see below) just using the grid from an icosphere.
	/*
	// #1: UV sphere integration
	auto tstep = 0.01 * pi;
	auto pstep = 0.01 * (pi - 0.002);
	for(auto phi = 0.001; phi < pi; phi += pstep) {
		for(auto theta = 0.f; theta < 2 * pi; theta += tstep) {
			auto a = float(std::sin(phi) * tstep * pstep);
			auto dir = nytl::Vec3d {
				cos(theta) * sin(phi),
				sin(theta) * sin(phi),
				cos(phi),
			};

			auto f = testFunc(dir);
			s.coeffs += a * f * tkn::projectSH9(dir).coeffs;
		}
	}
	*/

	// #2: ico integration
	// auto ico = tkn::generateIco(3);
	// auto numTris = ico.indices.size() / 3;
	// auto area = 4 * pi / numTris;
	// for(auto pos : ico.positions) {
	// 	auto f = testFunc(nytl::Vec3d(pos));
	// 	s.coeffs += area * f * tkn::projectSH9(pos).coeffs;
	// }

	// #3: monte-carlo integration using the hammersley sequence
	auto numSamples = 20 * 256u;
	for(auto i = 0u; i < numSamples; ++i) {
		auto h = hammersley(i, numSamples);
		float phi = 2.f * pi * h.x;
		auto cosTheta = 1.f - 2.f * h.y;
		auto sinTheta = std::sqrt(1.f - cosTheta * cosTheta);
		nytl::Vec3f pos{std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
		auto f = testFunc(nytl::Vec3d(pos));
		s.coeffs += f * tkn::projectSH9(pos).coeffs;
	}
	s.coeffs = float(4 * pi / numSamples) * s.coeffs;

	// dlg_info(evalIrradiance(s, Vec3f{0.f, 0.f, 1.f}));
	// dlg_info(eval(s, Vec3f{0.f, 0.f, 1.f}));

	auto fpi = float(pi);
	auto tests = {
		Vec2f{0.0f * fpi, 0.5f * fpi},
		Vec2f{0.2f * fpi, 0.5f * fpi},
		Vec2f{0.4f * fpi, 0.5f * fpi},
		Vec2f{1.0f * fpi, 0.5f * fpi},
		Vec2f{1.0f * fpi, 0.5f * fpi},
		Vec2f{1.5f * fpi, 0.5f * fpi},
		Vec2f{1.9f * fpi, 0.5f * fpi},

		Vec2f{0.f * fpi, 0.1f * fpi},
		Vec2f{0.5f * fpi, 0.1f * fpi},
		Vec2f{1.0f * fpi, 0.1f * fpi},
		Vec2f{1.5f * fpi, 0.1f * fpi},

		Vec2f{0.f * fpi, 0.9f * fpi},
		Vec2f{0.5f * fpi, 0.9f * fpi},
		Vec2f{1.0f * fpi, 0.9f * fpi},
		Vec2f{1.5f * fpi, 0.9f * fpi},

		Vec2f{1.921f * fpi, 0.343f * fpi},
		Vec2f{0.03241f * fpi, 0.8853f * fpi},
	};

	for(auto test : tests) {
		auto [theta, phi] = test;
		auto dir = nytl::Vec3d {
			cos(theta) * sin(phi),
			sin(theta) * sin(phi),
			cos(phi),
		};

		auto shv = eval(s, dir);
		auto ref = testFunc(dir);
		EXPECT(shv, nytl::approx(ref, 0.05));
		dlg_info("{}: {} vs {}", test, shv, ref);
	}
}
