// Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt.
// All rights reserved. See LICENSE in this folder for the full license.
// See https://github.com/selfshadow/ltc_code for the original code.

#pragma once

#include <tkn/types.hpp>
#include <tkn/math.hpp>
#include <nytl/matOps.hpp>
#include <cmath>

using namespace tkn::types;

struct LTC {
	// lobe magnitude
	float magnitude {1.f};

	// Average Schlick Fresnel term
	float fresnel {1.f};

	// parametric representation
	float m11 {1.f};
	float m22 {1.f};
	float m13 {0.f};
	Vec3f X {1.f, 0.f, 0.f};
	Vec3f Y {0.f, 1.f, 0.f};
	Vec3f Z {0.f, 0.f, 1.f};

	// matrix representation
	Mat3f M;
	Mat3f invM;
	float detM;

	LTC() { update(); }

	// compute matrix from parameters
	void update() {
		// M = transpose(Mat3f{X, Y, Z}) * nytl::Mat3f{
		// 	m11, 0, m13,
		// 	0, m22, 0,
		// 	0, 0, 1
		// };

		// transpose(X, Y, Z) * (m11, 0, m13, 0, m22, 0, 0, 0, 1)
		M = {
			m11 * X[0], m22 * Y[0], m13 * X[0] + Z[0],
			m11 * X[1], m22 * Y[1], m13 * X[1] + Z[1],
			m11 * X[2], m22 * Y[2], m13 * X[2] + Z[2]};
		invM = tkn::inverseAndDet(M, detM);
		detM = std::abs(detM);
	}

	float eval(const Vec3f& L) const {
		using nytl::constants::pi;
		const auto Loriginal = normalized(invM * L);
		const auto L_ = M * Loriginal;
		const float l = length(L_);
		const float Jacobian = detM / (l*l*l);
		const float D = 1.0f / nytl::constants::pi * std::max<float>(0.0f, Loriginal.z);
		const float res = magnitude * D / Jacobian;
		return res;
	}

	Vec3f sample(const float U1, const float U2) const {
		using nytl::constants::pi;
		const float theta = acosf(sqrtf(U1));
		const float phi = 2.0f * pi * U2;
		const auto dir = Vec3f{
			std::sin(theta) * std::cos(phi),
			std::sin(theta) * std::sin(phi),
			std::cos(theta)};
		return normalized(M * dir);
	}
};
