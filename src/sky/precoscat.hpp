// Implement E. Bruneton's per-calculated atmospheric scattering solution.
// https://ebruneton.github.io/precomputed_atmospheric_scattering/
// https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/functions.glsl.html
// We use some code from the sample implementation:
//
// Copyright (c) 2017 Eric Bruneton
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
// may be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#ifdef __cplusplus
	#include <cassert>
	#include <memory>

	#include <tkn/types.hpp>
	#include <tkn/glsl.hpp>
	using namespace tkn::glsl;
	using namespace tkn::types;

	#define IN(T) const T&
	#define OUT(T) T&

	// sorry, no other sane way.
	// Adding custom constructors to the vec class takes
	// its beautiful aggregrate property away
	#define vec2(x, y) vec2{x, y}
	#define vec3(x, y, z) vec3{x, y, z}
	#define vec4(x, y, z, w) vec4{x, y, z, w}
	#define uvec4(x, y, z, w) uvec4{x, y, z, w}

	nytl::Vec3f rgb(nytl::Vec4f vec) {
		return {vec[0], vec[1], vec[2]};
	}

	template<std::size_t D>
	struct sampler {
		nytl::Vec<D, unsigned> size;
		std::unique_ptr<nytl::Vec4f[]> data;
	};

	template<std::size_t D>
	nytl::Vec4f texture(const sampler<D>& sampler, nytl::Vec<D, float> coords) {
		// TODO: linear interpolation and such
		// we currently use clampToEdge addressing, nearest neighbor
		auto x = coords * sampler.size;
		x = clamp(x, Vec<D, float>{}, Vec<D, float>(sampler.size) - 1.f);
		auto u = Vec<D, unsigned>(x);
		auto id = 0u;
		for(auto i = 0u; i < D; ++i) {
			auto prod = 1u;
			for(auto j = 0u; j < i; ++j) {
				prod *= sampler.size[j];
			}
			id += prod * u[i];
		}

		return sampler.data[id];
	}

	template<std::size_t D>
	nytl::Vec<D, int> textureSize(const sampler<D>& sampler, int lod) {
		assert(lod == 0u);
		return Vec<D, int>(sampler.size);
	}

	using sampler1D = sampler<1>;
	using sampler2D = sampler<2>;
	using sampler3D = sampler<3>;
#else
	#define IN(T) in T
	#define OUT(T) out T
	#define assert(x)

	vec3 rgb(vec4 v) {
		return v.rgb;
	}
#endif

// Describes falloff of a group of particles throughout the atmosphere
struct Layer {
	float expTerm;
	float expScale;
	float lienarTerm;
	float constantTerm;
};

float density(IN(Layer) layer, float altitude) {
	float a = layer.expTerm * exp(layer.expScale * altitude);
	float b = layer.lienarTerm * altitude;
	return clamp(a + b + layer.constantTerm, 0.f, 1.f);
}

struct Atmosphere {
	float bottom; // lowest radius
	float top; // highest radius
	float sunAngularRadius;
	// The cosine of the maximum Sun zenith angle for which atmospheric scattering
	// must be precomputed (for maximum precision, use the smallest Sun zenith
	// angle yielding negligible sky light radiance values. For instance, for the
	// Earth case, 102 degrees is a good choice - yielding minMuS = -0.2).
	float minMuS;

	float mieG;
	float _pad1;
	float _pad2;
	float _pad3;

	vec4 mieExtinction;
	vec4 absorptionExtinction;

	Layer rayleighDensity;
	Layer mieDensity;
	Layer absorptionDensity;

	vec4 mieScattering;
	vec4 rayleighScattering; // same as rayleighExtinction
	vec4 solarIrradiance;
};

// Ray through all of the atmosphere
struct ARay {
	// distance from planet center, radius for start point
	// basically the start position of the ray, except that we can
	// ignore the other dimensions due to symmetry of the planet (sphere)
	float height;
	// cosine of angle between (origin-point) and (point-raydir)
	// basically the
	float mu;
};

// Returns whether the given ray intersects the ground (i.e. the bottom
// of the given atmosphere).
bool intersectsGround(IN(Atmosphere) atmos, IN(ARay) ray) {
	assert(ray.height >= atmos.bottom);
	assert(ray.mu <= 1.f && ray.mu >= -1.f);
	return ray.mu < 0.0 && ray.height * ray.height * (ray.mu * ray.mu - 1.0) +
		atmos.bottom * atmos.bottom >= 0.0;
}

// Returns the distance (along the ray) to its intersection with
// the bottom of the atmosphere. Not all rays intersect with
// the atmosphere! Check 'intersectsGround' before.
// Will return undefined value if there is no intersection.
float distanceToBottom(IN(Atmosphere) atmos, IN(ARay) ray) {
	assert(ray.height >= atmos.bottom);
	assert(ray.mu <= 1.f && ray.mu >= -1.f);
	float d = ray.height * ray.height * (ray.mu * ray.mu - 1.0) +
		atmos.bottom * atmos.bottom;
	return max(-ray.height * ray.mu - sqrt(max(d, 0.f)), 0.f);
}

// Returns the distance (along the ray) to its intersection with
// the top of the atmosphere. Every ray (starting inside the atmosphere,
// as we always assume) has it.
float distanceToTop(IN(Atmosphere) atmos, IN(ARay) ray) {
	assert(ray.height <= atmos.top);
	assert(ray.mu <= 1.f && ray.mu >= -1.f);
	float d = ray.height * ray.height * (ray.mu * ray.mu - 1.0) +
		atmos.top * atmos.top;
	return max(-ray.height * ray.mu + sqrt(max(d, 0.f)), 0.f);
}

// Computes the optical depth of the given ray through the given
// layer to the top of the atmosphere.
float opticalDepth(IN(Atmosphere) atmos, IN(Layer) layer, IN(ARay) ray) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu <= 1.f && ray.mu >= -1.f);
	float r = ray.height;

	// NOTE: might be good idea to make sampleCount depend on length
	// of ray
	const uint sampleCount = 500u;
	float dt = distanceToTop(atmos, ray) / sampleCount;
	float accum = 0.f;
	for(uint i = 0u; i <= sampleCount; ++i) {
		float t_i = i * dt;
		float r_i = sqrt(t_i * t_i + 2.f * r * ray.mu * t_i + r * r);
		float d_i = density(layer, r_i - atmos.bottom);
		float w_i = (i == 0u || i == sampleCount) ? 0.5f : 1.f;
		accum += dt * w_i * d_i;
	}

	return accum;
}

// Computes the optical depths of the given ray through to the top
// of the atmosphere for the rayleigh, mie and absorption layers.
// Might be more efficient to calculate all three at once, given the
// cheap operation and high sample count.
vec3 opticalDepths(IN(Atmosphere) atmos, IN(ARay) ray) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu <= 1.f && ray.mu >= -1.f);
	float r = ray.height;

	// NOTE: might be good idea to make sampleCount depend on length
	// of ray
	const uint sampleCount = 500u;
	float dt = distanceToTop(atmos, ray) / sampleCount;
	vec3 accum = vec3(0.f, 0.f, 0.f);
	for(uint i = 0u; i <= sampleCount; ++i) {
		float t_i = i * dt;
		float r_i = sqrt(t_i * t_i + 2.f * r * ray.mu * t_i + r * r);
		float dr_i = density(atmos.rayleighDensity, r_i - atmos.bottom);
		float dm_i = density(atmos.mieDensity, r_i - atmos.bottom);
		float da_i = density(atmos.absorptionDensity, r_i - atmos.bottom);
		float w_i = (i == 0u || i == sampleCount) ? 0.5f : 1.f;
		accum += dt * w_i * vec3(dr_i, dm_i, da_i);
	}

	return accum;
}

vec3 transmittanceToTop(IN(Atmosphere) atmos, IN(ARay) ray) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.0 && ray.mu <= 1.0);

	vec3 depths = opticalDepths(atmos, ray);
	return exp(-(rgb(atmos.rayleighScattering) * depths.x +
				rgb(atmos.mieExtinction) * depths.y +
				rgb(atmos.absorptionExtinction) * depths.z));
}

// basically rescales coordinates over texture.
float uvFromUnitRange(float range, float size) {
	return 0.5f / size + range * (1.f - 1.f / size);
}

vec2 uvFromUnitRange(vec2 range, ivec2 size) {
	return 0.5f / size + range * (1.f - 1.f / size);
}

vec4 uvFromUnitRange(vec4 range, ivec4 size) {
	return 0.5f / size + range * (1.f - 1.f / size);
}

vec2 transmittanceUnitFromRay(IN(Atmosphere) atmos, IN(ARay) ray) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.0 && ray.mu <= 1.0);

	// Distance to top atmosphere boundary for a horizontal ray at ground level.
	float H = sqrt(atmos.top * atmos.top - atmos.bottom * atmos.bottom);
	// Distance to the horizon.
	float rho = sqrt(max(ray.height * ray.height - atmos.bottom * atmos.bottom, 0.f));
	// Distance to the top atmosphere boundary for the ray (r,mu), and its minimum
	// and maximum values over all mu - obtained for (r,1) and (r,mu_horizon).
	float d = distanceToTop(atmos, ray);
	float d_min = atmos.top - ray.height;
	float d_max = rho + H;
	float x_mu = (d - d_min) / (d_max - d_min);
	float x_r = rho / H;
	return vec2(x_mu, x_r);
}

ARay transmittanceRayFromUnit(IN(Atmosphere) atmos, IN(vec2) range) {
	float x_mu = range.x;
	float x_r = range.y;
	// Distance to top atmosphere boundary for a horizontal ray at ground level.
	float H = sqrt(atmos.top * atmos.top - atmos.bottom * atmos.bottom);
	// Distance to the horizon, from which we can compute r:
	float rho = H * x_r;
	float r = sqrt(rho * rho + atmos.bottom * atmos.bottom);
	// Distance to the top atmosphere boundary for the ray (r,mu), and its minimum
	// and maximum values over all mu - obtained for (r,1) and (r,mu_horizon) -
	// from which we can recover mu:
	float d_min = atmos.top - r;
	float d_max = rho + H;
	float d = d_min + x_mu * (d_max - d_min);
	float mu = (d == 0.0) ? 1.f : (H * H - rho * rho - d * d) / (2.f * r * d);
	mu = clamp(mu, -1.f, 1.f);

	ARay res = {r, mu};
	return res;
}

// lookup
vec3 transmittanceToTop(IN(Atmosphere) atmos, IN(sampler2D) transTex, IN(ARay) ray) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.f && ray.mu <= 1.f);
	vec2 unit = transmittanceUnitFromRay(atmos, ray);
	vec2 uv = uvFromUnitRange(unit, textureSize(transTex, 0));
	return rgb(texture(transTex, uv));
}

vec3 transmittance(IN(Atmosphere) atmos, IN(sampler2D) transTex, IN(ARay) ray,
		float depth, bool rayIntersectsGround) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.f && ray.mu <= 1.f);
	assert(depth >= 0.0);

	float r = ray.height;
	float mu = ray.mu;
	float r_d = clamp(sqrt(depth * depth + 2.f * r * mu * depth + r * r),
		atmos.bottom, atmos.top);
	float mu_d = clamp((r * mu + depth) / r_d, -1.f, 1.f);

	ARay full, nonrelevant;
	if(rayIntersectsGround) {
		full.height = r_d;
		full.mu = -mu_d;
		nonrelevant.height = r;
		nonrelevant.mu = -mu;
	} else {
		full.height = r;
		full.mu = mu;
		nonrelevant.height = r_d;
		nonrelevant.mu = mu_d;
	}

	// the nonrelevant part (from the second point, q, to the top
	// of the atmosphere) should always be a subpart of the full
	// path (the givene ray to the top of the atmosphere), therefore
	// the transmittage on the full path smaller. Therefore this
	// should naturally return something <= 1.f.
	// I guess the min is needed for some corner cases.
	return min(vec3(1.f, 1.f, 1.f),
			transmittanceToTop(atmos, transTex, full) /
			transmittanceToTop(atmos, transTex, nonrelevant));
}

vec3 transmittanceToSun(IN(Atmosphere) atmos, IN(sampler2D) transTex,
		IN(ARay) toSun) {
	vec3 trans = transmittanceToTop(atmos, transTex, toSun);

	// fraction of the sun above the horizon
	float st = atmos.bottom / toSun.height;
	float ct = -sqrt(max(0.f, 1.f - st * st));
	float above = smoothstep(
		-st * atmos.sunAngularRadius,
		st * atmos.sunAngularRadius, toSun.mu - ct);
	return above * trans;
}

// scattering
// Returns the amount of mie and rayleigh scattering (separately) for
// the given 'ray' at its depth 'd'.
// mu_s is the cos(angle) between a vector to the starting point of the ray
// and the direction vector to the sun.
// nu is the cos(angle) between the direction of the ray and the direction
// vector to the sun.
void singleScatteringIntegrand(IN(Atmosphere) atmos, IN(sampler2D) transTex,
		IN(ARay) ray, float mu_s, float nu, float d,
		bool rayIntersectsGround, OUT(vec3) rayleigh, OUT(vec3) mie) {
	float r = ray.height;
	float mu = ray.mu;
	float r_d = clamp(sqrt(d * d + 2.f * r * mu * d + r * r), atmos.bottom, atmos.top);
	float mu_s_d = clamp((r * mu_s + d * nu) / r_d, -1.f, 1.f);

	ARay q2Sun = {r_d, mu_s_d};
	vec3 trans =
		transmittance(atmos, transTex, ray, d, rayIntersectsGround) *
		transmittanceToSun(atmos, transTex, q2Sun);

	rayleigh = trans * density(atmos.rayleighDensity, r_d - atmos.bottom);
	mie = trans * density(atmos.mieDensity, r_d - atmos.bottom);
}

float distanceToNearestBoundary(IN(Atmosphere) atmos, IN(ARay) ray,
		bool rayIntersectsGround) {
	return rayIntersectsGround ?
		distanceToBottom(atmos, ray) :
		distanceToTop(atmos, ray);
}

void singleScattering(IN(Atmosphere) atmos, IN(sampler2D) transTex,
		IN(ARay) ray, float mu_s, float nu,
		bool rayIntersectsGround, OUT(vec3) rayleigh, OUT(vec3) mie) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.0 && ray.mu <= 1.0);
	assert(mu_s >= -1.0 && mu_s <= 1.0);
	assert(nu >= -1.0 && nu <= 1.0);

	const uint sampleCount = 50;
	float dt = distanceToNearestBoundary(atmos, ray, rayIntersectsGround) / sampleCount;

	rayleigh = vec3(0.f, 0.f, 0.f);
	mie = vec3(0.f, 0.f, 0.f);
	for(uint i = 0u; i <= sampleCount; ++i) {
		float t_i = i * dt;
		vec3 rayleigh_i, mie_i;
		singleScatteringIntegrand(atmos, transTex, ray, mu_s, nu, t_i,
			rayIntersectsGround, rayleigh_i, mie_i);

		// sample weight (from the trapezoidal rule).
		float w_i = (i == 0 || i == sampleCount) ? 0.5 : 1.0;
		rayleigh += w_i * rayleigh_i;
		mie += w_i * mie_i;
	}

	vec3 s = dt * rgb(atmos.solarIrradiance);
	rayleigh *= s * rgb(atmos.rayleighScattering);
	mie *= s * rgb(atmos.mieScattering);
}

// used at rendering
float phase(float nu, float g) {
	float gg = g * g;

	const float fac = 0.11936620731; // 3 / (8 * pi) for normalization
	float nu2 = nu * nu;
	return fac * ((1 - gg) * (1 + nu2)) / ((2 + gg) * pow(1 + gg - 2 * g * nu, 1.5f));
}

// phase function with g = 0, optimized
float phaseRayleigh(float nu) {
	const float fac = 0.05968310365; // 3 / (16 * pi) for normalization
	return fac * (1 + nu * nu);
}

float unit2Half(float x, uint texSize) {
	return x * (texSize - 1) / (texSize - 2);
}

vec4 scatTexUVFromParams(IN(Atmosphere) atmos, IN(ARay) ray,
		float mu_s, float nu, bool rayIntersectsGround, IN(uvec4) texSize) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.0 && ray.mu <= 1.0);
	assert(mu_s >= -1.0 && mu_s <= 1.0);
	assert(nu >= -1.0 && nu <= 1.0);

	float r = ray.height;
	float mu = ray.mu;

	// Distance to top atmosphere boundary for a horizontal ray at ground level.
	float H = sqrt(atmos.top * atmos.top - atmos.bottom * atmos.bottom);
	// Distance to the horizon.
	float rho = sqrt(max(0.f, r * r - atmos.bottom * atmos.bottom));
	float u_r = uvFromUnitRange(rho / H, texSize[3]);

	// Discriminant of the quadratic equation for the intersections of the ray
	// (r,mu) with the ground (see RayIntersectsGround).
	float r_mu = r * mu;
	float discriminant = r_mu * r_mu - r * r + atmos.bottom * atmos.bottom;
	float u_mu;
	if(rayIntersectsGround) {
		// Distance to the ground for the ray (r,mu), and its minimum and maximum
		// values over all mu - obtained for (r,-1) and (r,mu_horizon).
		float d = -r_mu - sqrt(max(0.f, discriminant));
		float d_min = r - atmos.bottom;
		float d_max = rho;
		u_mu = 0.5 - 0.5 * (d_max == d_min ? 0.0 :
			uvFromUnitRange((d - d_min) / (d_max - d_min), texSize[2] / 2));
	} else {
		// Distance to the top atmosphere boundary for the ray (r,mu), and its
		// minimum and maximum values over all mu - obtained for (r,1) and
		// (r,mu_horizon).
		float d = -r_mu + sqrt(max(0.f, discriminant + H * H));
		float d_min = atmos.top - r;
		float d_max = rho + H;
		u_mu = 0.5 + 0.5 *
			uvFromUnitRange((d - d_min) / (d_max - d_min), texSize[2] / 2);
	}

	ARay toSun = {atmos.bottom, mu_s};
	float d = distanceToTop(atmos, toSun);
	float d_min = atmos.top - atmos.bottom;
	float d_max = H;
	float a = (d - d_min) / (d_max - d_min);
	float A = -2.f * atmos.minMuS * atmos.bottom / (d_max - d_min);
	float u_mu_s = uvFromUnitRange(max(1.f - a / A, 0.f) / (1.f + a), texSize[1]);

	float u_nu = uvFromUnitRange((nu + 1.0) / 2.0, texSize[0]);
	return vec4(u_nu, u_mu_s, u_mu, u_r);
}

void scatParamsFromTexUnit(IN(Atmosphere) atmos, vec4 unit, OUT(ARay) ray,
		OUT(float) mu_s, OUT(float) nu, OUT(bool) rayIntersectsGround,
		IN(uint) muTexSize) {

	assert(clamp(unit, 0.f, 1.f) == unit);

	// Distance to top atmosphere boundary for a horizontal ray at ground level.
	float H = sqrt(atmos.top * atmos.top - atmos.bottom * atmos.bottom);
	// Distance to the horizon.
	float rho = H * unit[3];
	float r = ray.height = sqrt(rho * rho + atmos.bottom * atmos.bottom);

	if(unit[2] < 0.5) {
		// Distance to the ground for the ray (r,mu), and its minimum and maximum
		// values over all mu - obtained for (r,-1) and (r,mu_horizon) - from which
		// we can recover mu:
		float d_min = r - atmos.bottom;
		float d_max = rho;
		float d = d_min + (d_max - d_min) * unit2Half(1.f - 2.f * unit[2], muTexSize / 2);
		ray.mu = (d == 0.0) ? -1.f :
			clamp(-(rho * rho + d * d) / (2.f * r * d), -1.f, 1.f);
		rayIntersectsGround = true;
	} else {
		// Distance to the top atmosphere boundary for the ray (r,mu), and its
		// minimum and maximum values over all mu - obtained for (r,1) and
		// (r,mu_horizon) - from which we can recover mu:
		float d_min = atmos.top - r;
		float d_max = rho + H;
		float d = d_min + (d_max - d_min) * unit2Half(2.f * unit[2] - 1.f, muTexSize / 2);
		ray.mu = (d == 0.0) ? 1.f :
			clamp((H * H - rho * rho - d * d) / (2.f * r * d), -1.f, 1.f);
		rayIntersectsGround = false;
	}

	float x_mu_s = unit[1];

	float d_min = atmos.top - atmos.bottom;
	float d_max = H;
	float A = -2.f * atmos.minMuS * atmos.bottom / (d_max - d_min);
	float a = (A - x_mu_s * A) / (1.0 + x_mu_s * A);
	float d = d_min + min(a, A) * (d_max - d_min);
	mu_s = (d == 0.0) ? 1.f :
		clamp((H * H - d * d) / (2.f * atmos.bottom * d), -1.f, 1.f);

	nu = clamp(2.f * unit[0] - 1.f, -1.f, 1.f);
}

// quad-linear interpolated lookup
vec3 getScattering(IN(Atmosphere) atmos,
		IN(sampler3D) scatMieTex, IN(sampler3D) scatRayleighTex,
		IN(ARay) ray, float mu_s, float nu,
		bool rayIntersectsGround, uint scatNuSize) {

	uvec3 tsize = uvec3(textureSize(scatMieTex, 0));
	uvec4 usize = uvec4(scatNuSize, tsize[0] / scatNuSize,
		tsize[1], tsize[2]);

	vec4 unit = scatTexUVFromParams(atmos, ray, mu_s, nu,
		rayIntersectsGround, usize);

	// unit[1] = uvFromUnitRange(unit[1], tsize.x / scatNuSize);
	// unit[2] = uvFromUnitRange(unit[2], tsize.y);
	// unit[3] = uvFromUnitRange(unit[3], tsize.z);

	float x = (scatNuSize - 1) * unit[0];
	float x0 = floor(x);
	float lerp = x - x0;

	vec3 uvw0 = vec3((x0 + unit[1]) / scatNuSize, unit[2], unit[3]);
	vec3 uvw1 = vec3((x0 + 1.f + unit[1]) / scatNuSize, unit[2], unit[3]);

	// vec3 m = rgb(texture(scatMieTex, uvw0));
	vec3 m = rgb(mix(texture(scatMieTex, uvw0), texture(scatMieTex, uvw1), lerp));
	m *= phase(nu, atmos.mieG);
	// vec3 r = rgb(texture(scatRayleighTex, uvw0));
	vec3 r = rgb(mix(texture(scatRayleighTex, uvw0), texture(scatRayleighTex, uvw1), lerp));
	r *= phaseRayleigh(nu);

	return r + m;
}
