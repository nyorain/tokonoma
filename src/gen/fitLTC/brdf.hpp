// Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt.
// All rights reserved. See LICENSE in this folder for the full license.
// See https://github.com/selfshadow/ltc_code for the original code.

#pragma once

#include <tkn/types.hpp>
#include <cmath>
using namespace tkn::types;

struct Brdf {
	virtual ~Brdf() = default;

    // evaluation of the cosine-weighted BRDF
    // pdf is set to the PDF of sampling L
    virtual float eval(const Vec3f& V, const Vec3f& L,
		float alpha, float& pdf) const = 0;
    virtual Vec3f sample(const Vec3f& V, float alpha,
		float U1, float U2) const = 0;
};

struct BrdfGGX : public Brdf {
    virtual float eval(const Vec3f& V, const Vec3f& L,
			float alpha, float& pdf) const {
        if(V.z <= 0) {
            pdf = 0;
            return 0;
        }

        // masking
        const float LambdaV = lambda(alpha, V.z);

        // shadowing
        float G2;
        if(L.z <= 0.0f) {
            G2 = 0;
		} else {
            const float LambdaL = lambda(alpha, L.z);
            G2 = 1.0f/(1.0f + LambdaV + LambdaL);
        }

        // D
        const auto H = normalized(V + L);
        const float slopex = H.x/H.z;
        const float slopey = H.y/H.z;
        float D = 1.0f / (1.0f + (slopex*slopex + slopey*slopey)/alpha/alpha);
        D = D*D;
        D = D/(3.14159f * alpha*alpha * H.z*H.z*H.z*H.z);

        pdf = fabsf(D * H.z / 4.0f / dot(V, H));
        float res = D * G2 / 4.0f / V.z;

        return res;
    }

    virtual Vec3f sample(const Vec3f& V, float alpha, float U1, float U2) const {
        const float phi = 2.0f*3.14159f * U1;
        const float r = alpha*sqrtf(U2/(1.0f - U2));
		const auto N = normalized(Vec3f{r * std::cos(phi), r * std::sin(phi), 1.0f});
        const auto L = -V + 2.0f * dot(N, V) * N;
        return L;
    }

    float lambda(const float alpha, const float cosTheta) const {
        const float a = 1.0f / alpha / tanf(acosf(cosTheta));
        return (cosTheta < 1.0f) ? 0.5f * (-1.0f + sqrtf(1.0f + 1.0f/a/a)) : 0.0f;
    }
};
