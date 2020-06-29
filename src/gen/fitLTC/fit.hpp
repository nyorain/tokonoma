// Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt.
// All rights reserved. See LICENSE in this folder for the full license.
// See https://github.com/selfshadow/ltc_code for the original code.

#pragma once

#include "ltc.hpp"
#include "brdf.hpp"
#include "nelderMead.hpp"

#include <tkn/types.hpp>
#include <tkn/math.hpp>
#include <nytl/math.hpp>
#include <cmath>
#include <iostream>

using nytl::constants::pi;
using namespace tkn::types;
using std::sin;
using std::cos;

// number of samples used to compute the error during fitting
const int Nsample = 32;
// minimal roughness (avoid singularities)
const float MIN_ALPHA = 0.00001f;

// computes
// * the norm (albedo) of the BRDF
// * the average Schlick Fresnel value
// * the average direction of the BRDF
void computeAvgTerms(const Brdf& brdf, const Vec3f& V, const float alpha,
		float& norm, float& fresnel, Vec3f& averageDir) {
    norm = 0.0f;
    fresnel = 0.0f;
    averageDir = {};

    for(int j = 0; j < Nsample; ++j)
    for(int i = 0; i < Nsample; ++i) {
        const float U1 = (i + 0.5f)/Nsample;
        const float U2 = (j + 0.5f)/Nsample;

        // sample
        const auto L = brdf.sample(V, alpha, U1, U2);

        // eval
        float pdf;
        float eval = brdf.eval(V, L, alpha, pdf);

        if(pdf > 0) {
            float weight = eval / pdf;
            auto H = normalized(V+L);

            // accumulate
            norm       += weight;
            fresnel    += weight * pow(1.0f - std::max(dot(V, H), 0.0f), 5.0f);
            averageDir += weight * L;
        }
    }

    norm    /= (float)(Nsample*Nsample);
    fresnel /= (float)(Nsample*Nsample);

    // clear y component, which should be zero with isotropic BRDFs
    averageDir.y = 0.0f;
    averageDir = normalized(averageDir);
}

// compute the error between the BRDF and the LTC
// using Multiple Importance Sampling
float computeError(const LTC& ltc, const Brdf& brdf, const Vec3f& V,
		const float alpha) {
    double error = 0.0;

    for (int j = 0; j < Nsample; ++j)
    for (int i = 0; i < Nsample; ++i) {
        const float U1 = (i + 0.5f)/Nsample;
        const float U2 = (j + 0.5f)/Nsample;

        // importance sample LTC
        {
            // sample
            const auto L = ltc.sample(U1, U2);

            float pdf_brdf;
            float eval_brdf = brdf.eval(V, L, alpha, pdf_brdf);
            float eval_ltc = ltc.eval(L);
            float pdf_ltc = eval_ltc/ltc.magnitude;

            // error with MIS weight
            double error_ = fabsf(eval_brdf - eval_ltc);
            error_ = error_*error_*error_;
            error += error_/(pdf_ltc + pdf_brdf);
        }

        // importance sample BRDF
        {
            // sample
            const auto L = brdf.sample(V, alpha, U1, U2);

            float pdf_brdf;
            float eval_brdf = brdf.eval(V, L, alpha, pdf_brdf);
            float eval_ltc = ltc.eval(L);
            float pdf_ltc = eval_ltc/ltc.magnitude;

            // error with MIS weight
            double error_ = fabsf(eval_brdf - eval_ltc);
            error_ = error_*error_*error_;
            error += error_/(pdf_ltc + pdf_brdf);
        }
    }

    return (float)error / (float)(Nsample*Nsample);
}

struct FitLTC {
    FitLTC(LTC& ltc_, const Brdf& brdf, bool isotropic_, const Vec3f& V_, float alpha_) :
        ltc(ltc_), brdf(brdf), V(V_), alpha(alpha_), isotropic(isotropic_)
    {
    }

    void update(const float* params)
    {
        float m11 = std::max<float>(params[0], 1e-7f);
        float m22 = std::max<float>(params[1], 1e-7f);
        float m13 = params[2];

        if(isotropic) {
            ltc.m11 = m11;
            ltc.m22 = m11;
            ltc.m13 = 0.0f;
        } else {
			ltc.m11 = m11;
            ltc.m22 = m22;
            ltc.m13 = m13;
        }
        ltc.update();
    }

    float operator()(const float* params) {
        update(params);
        return computeError(ltc, brdf, V, alpha);
    }

    LTC& ltc;
    const Brdf& brdf;
    const Vec3f& V;
    float alpha;
    bool isotropic;
};

// fit brute force
// refine first guess by exploring parameter space
void fit(LTC& ltc, const Brdf& brdf, const Vec3f& V, const float alpha,
		const float epsilon = 0.05f, const bool isotropic = false) {
    float startFit[3] = { ltc.m11, ltc.m22, ltc.m13 };
    float resultFit[3];

    FitLTC fitter(ltc, brdf, isotropic, V, alpha);

    // Find best-fit LTC lobe (scale, alphax, alphay)
    float error = NelderMead<3>(resultFit, startFit, epsilon, 1e-5f, 100, fitter);
	(void) error;

    // Update LTC with best fitting values
    fitter.update(resultFit);
}

// fit data
void fitTab(Mat3f* tab, Vec2f* tabMagFresnel, const int N, const Brdf& brdf) {
    LTC ltc;

    // loop over theta and alpha
    for(int a = N - 1; a >=     0; --a)
    for(int t =     0; t <= N - 1; ++t) {

        // parameterised by sqrt(1 - cos(theta))
        float x = t/float(N - 1);
        float ct = 1.0f - x*x;
        float theta = std::min<float>(1.57f, acosf(ct));
        const Vec3f V = Vec3f{std::sin(theta), 0, std::cos(theta)};

        // alpha = roughness^2
        float roughness = a/float(N - 1);
        float alpha = std::max<float>(roughness*roughness, MIN_ALPHA);

		using std::cout;
		using std::endl;
        cout << "a = " << a << "\t t = " << t  << endl;
        cout << "alpha = " << alpha << "\t theta = " << theta << endl;
        cout << endl;

        Vec3f averageDir;
        computeAvgTerms(brdf, V, alpha, ltc.magnitude, ltc.fresnel, averageDir);

        bool isotropic;

        // 1. first guess for the fit
        // init the hemisphere in which the distribution is fitted
        // if theta == 0 the lobe is rotationally symmetric and aligned with Z = (0 0 1)
        if (t == 0) {
            ltc.X = Vec3f{1, 0, 0};
            ltc.Y = Vec3f{0, 1, 0};
            ltc.Z = Vec3f{0, 0, 1};

            if(a == N - 1) {
				// roughness = 1
                ltc.m11 = 1.0f;
                ltc.m22 = 1.0f;
            } else {
				// init with roughness of previous fit
                ltc.m11 = tab[a + 1 + t*N][0][0];
                ltc.m22 = tab[a + 1 + t*N][1][1];
            }

            ltc.m13 = 0;
            ltc.update();
            isotropic = true;
        } else {
        	// otherwise use previous configuration as first guess
            auto L = averageDir;
            auto T1 = Vec3f{L.z, 0, -L.x};
            auto T2 = Vec3f{0, 1, 0};
            ltc.X = T1;
            ltc.Y = T2;
            ltc.Z = L;

            ltc.update();

            isotropic = false;
        }

        // 2. fit (explore parameter space and refine first guess)
        float epsilon = 0.05f;
        fit(ltc, brdf, V, alpha, epsilon, isotropic);

        // copy data
        tab[a + t*N] = ltc.M;
        tabMagFresnel[a + t*N][0] = ltc.magnitude;
        tabMagFresnel[a + t*N][1] = ltc.fresnel;

        // kill useless coefs in matrix
        tab[a+t*N][1][0] = 0;
        tab[a+t*N][0][1] = 0;
        tab[a+t*N][1][2] = 0;
        tab[a+t*N][2][1] = 0;

		cout << tab[a + t * N] << endl;
        cout << endl;
    }
}

float sqr(float x) {
    return x*x;
}

float G(float w, float s, float g) {
    return -2.0f*sin(w)*cos(s)*cos(g) + pi/2.0f - g + sin(g)*cos(g);
}

float H(float w, float s, float g) {
    float sinsSq = sqr(sin(s));
    float cosgSq = sqr(cos(g));

    return cosf(w)*(cosf(g)*sqrtf(sinsSq - cosgSq) + sinsSq*asinf(cosf(g)/sinf(s)));
}

float ihemi(float w, float s) {
    float g = asinf(cosf(s)/sinf(w));
    float sinsSq = sqr(sinf(s));

    if (w >= 0.0f && w <= (pi/2.0f - s)) {
        return pi*cosf(w)*sinsSq;
	}

    if (w >= (pi/2.0f - s) && w < pi/2.0f) {
        return pi*cosf(w)*sinsSq + G(w, s, g) - H(w, s, g);
	}

    if (w >= pi/2.0f && w < (pi/2.0f + s)) {
        return G(w, s, g) + H(w, s, g);
	}

    return 0.0f;
}

void genSphereTab(float* tabSphere, int N) {
    for(int j = 0; j < N; ++j)
    for(int i = 0; i < N; ++i) {
        const float U1 = float(i)/(N - 1);
        const float U2 = float(j)/(N - 1);

        // z = cos(elevation angle)
        float z = 2.0f*U1 - 1.0f;

        // length of average dir., proportional to sin(sigma)^2
        float len = U2;

        float sigma = asinf(sqrtf(len));
        float omega = acosf(z);

        // compute projected (cosine-weighted) solid angle of spherical cap
        float value = 0.0f;

        if (sigma > 0.0f) {
            value = ihemi(omega, sigma)/(pi*len);
		} else {
            value = std::max<float>(z, 0.0f);
		}

        if (value != value) {
            printf("nan!\n");
		}

        tabSphere[i + j*N] = value;
    }
}

void packTab(Vec4f* tex1, Vec4f* tex2, const Mat3f* tab,
		const Vec2f* tabMagFresnel, const float* tabSphere, int N) {
    for(int i = 0; i < N*N; ++i) {
        const Mat3f& m = tab[i];
        auto invM = tkn::inverse(m);

        // normalize by the middle element
        invM *= 1.f / invM[1][1];

        // store the variable terms
        tex1[i] = {invM[0][0], invM[2][0], invM[0][2], invM[2][2]};
        tex2[i] = {tabMagFresnel[i][0], tabMagFresnel[i][1], 0.f, tabSphere[i]};
    }
}
