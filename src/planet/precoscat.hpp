// file is directly taken from tkn/sky/bruneton.hpp.
// Those two should be merged at some point and moved to libtkn.

// Implements E. Bruneton's pre-calculated atmospheric scattering solution.
// https://ebruneton.github.io/precomputed_atmospheric_scattering/
// https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/functions.glsl.html
// We use most of the code from the sample implementation, with only
// slight changes.
//
// Copyright (c) 2017 Eric Bruneton
// Copyright (c) 2020 Jan Kelling
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

// The major interesting points about this implementations:
// - the way rays and scattering is parameterized. The graphs on
//   E. Bruneton's website are good for understand how a ray through
//   the atmosphere (ARay with r, mu), the sun direction (mu_s),
//   and angle between ray and sun direction (nu) are defined.
// - the way the data is written and read from textures.
//   The mapping onto/from texture coordinates is not trivial and optimized
//   to give precision where it is needed, allowing to keep the LUTs
//   as small as possible (which is needed, given for scattering they
//   have 4 dimensions).
// - optimizations in pre calculation, especially for multi scattering.
// - how we can retrieve scattering and transmission on a ray segment
//   though the atmosphere (defined by start and end point) by using
//   multiple respective texture lookups.

// Changes done from Bruneton's implementation:
// - change the way we store transmittance. Bruneton needs a 32-bit floating
//   point texture, since we need great precision near tramission ~ 0 for rays
//   with mu ~ 0, i.e. near the horizon. We get away with 16-bit by gamma-mapping it.
//   NOTE: nvm this does not seem too effective.

// NOTE: some calculations currently use (0, 0, 1) as zenith internally.
//   Although this does not match the orientation we usually use, it does not
//   matter since the vector representation is only a utility anyways, in the
//   end everything is converted from/to the cos(angle) representation that
//   is coordinate-system agnostic.

// TODO: optimization/improvement ideas
// - when integrating over spheres/hemispheres, we could use a low discprepancy
//   sequence instead of the current elevation/azimuth iteration that
//   has a lot more samples near the poles.

#include "glsl.hpp"

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
	uint scatNuSize;
	float absorptionPeak;
	float _pad3;

	vec4 mieExtinction;
	vec4 absorptionExtinction;

	Layer rayleighDensity;
	Layer mieDensity;
	Layer absorptionDensity0;
	Layer absorptionDensity1;

	vec4 mieScattering;
	vec4 rayleighScattering; // same as rayleighExtinction
	vec4 solarIrradiance;
	vec4 groundAlbedo;
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
		float off_i = sqrt(t_i * t_i + 2.f * r * ray.mu * t_i + r * r) - atmos.bottom;
		float dr_i = density(atmos.rayleighDensity, off_i);
		float dm_i = density(atmos.mieDensity, off_i);
		float da_i;
		if(off_i > atmos.absorptionPeak) {
			da_i = density(atmos.absorptionDensity1, off_i);
		} else {
			da_i = density(atmos.absorptionDensity0, off_i);
		}
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

vec2 transTexUnitFromRay(IN(Atmosphere) atmos, IN(ARay) ray) {
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

ARay rayFromTransTexUnit(IN(Atmosphere) atmos, IN(vec2) range) {
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
const float tGamma = 1.0;
vec3 transmittanceToTop(IN(Atmosphere) atmos, IN(sampler2D) transTex, IN(ARay) ray) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.f && ray.mu <= 1.f);
	vec2 unit = transTexUnitFromRay(atmos, ray);
	vec2 uv = uvFromUnitRange(unit, textureSize(transTex, 0));
	return pow(rgb(texture(transTex, uv)), vec3(tGamma, tGamma, tGamma));
}

vec3 getTransmittance(IN(Atmosphere) atmos, IN(sampler2D) transTex, IN(ARay) ray,
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
		getTransmittance(atmos, transTex, ray, d, rayIntersectsGround) *
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

	const uint sampleCount = 200;
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

	// Not 100% sure the above is correct, this code
	// emulates gl_FragCoord, like used by bruneton.
	// x *= (texSize - 1);
	// x += 0.5;
	// x /= texSize;
	// return (x - 0.5 / texSize) / (1.0 - 1.0 / texSize);
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
		u_mu = 0.5 - 0.5 * (d_max == d_min ? 0.0 : uvFromUnitRange((d - d_min) / (d_max - d_min), texSize[2] / 2));
	} else {
		// Distance to the top atmosphere boundary for the ray (r,mu), and its
		// minimum and maximum values over all mu - obtained for (r,1) and
		// (r,mu_horizon).
		float d = -r_mu + sqrt(max(0.f, discriminant + H * H));
		float d_min = atmos.top - r;
		float d_max = rho + H;
		u_mu = 0.5 + 0.5 * uvFromUnitRange((d - d_min) / (d_max - d_min), texSize[2] / 2);
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

void scatParamsFromPixel(IN(Atmosphere) atmos, uvec3 pixel, uvec3 texSize,
		OUT(ARay) ray, OUT(float) mu_s, OUT(float) nu,
		OUT(bool) rayIntersectsGround) {
	// assert(texSize.x % atmos.scatNuSize == 0);
	uint texMuSSize = texSize.x / atmos.scatNuSize;

	uint pixNu = pixel.x / texMuSSize; // floor
	uint pixMuS = uint(mod(pixel.x, texMuSSize));
	uvec4 pixel4 = uvec4(pixNu, pixMuS, pixel.y, pixel.z);
	vec4 max4 = vec4(atmos.scatNuSize, texMuSSize, texSize.y, texSize.z) - 1.f;
	vec4 unitRange = pixel4 / max4;
	scatParamsFromTexUnit(atmos, unitRange, ray, mu_s, nu,
		rayIntersectsGround, texSize.y);

	float mu = ray.mu;
	nu = clamp(nu,
		mu * mu_s - sqrt((1.f - mu * mu) * (1.f - mu_s * mu_s)),
		mu * mu_s + sqrt((1.f - mu * mu) * (1.f - mu_s * mu_s)));
}


// Quad-linear interpolated lookup for scattering.
void lerpScatteringCoords(IN(Atmosphere) atmos, ivec3 scatTexSize,
		IN(ARay) ray, float mu_s, float nu, bool rayIntersectsGround,
		OUT(vec3) uvw0, OUT(vec3) uvw1, OUT(float) lerp) {
	uvec3 tsize = uvec3(scatTexSize);
	uvec4 usize = uvec4(atmos.scatNuSize, tsize[0] / atmos.scatNuSize, tsize[1], tsize[2]);
	vec4 unit = scatTexUVFromParams(atmos, ray, mu_s, nu, rayIntersectsGround, usize);

	float x = (atmos.scatNuSize - 1) * unit[0];
	float x0 = floor(x);
	lerp = x - x0;

	uvw0 = vec3((x0 + unit[1]) / atmos.scatNuSize, unit[2], unit[3]);
	uvw1 = vec3((x0 + 1.f + unit[1]) / atmos.scatNuSize, unit[2], unit[3]);
}

// All textures must have the same extent.
// Single scattering
vec3 getScattering(IN(Atmosphere) atmos,
		IN(sampler3D) singleRScatTex, IN(sampler3D) singleMScatTex,
		IN(ARay) ray, float mu_s, float nu, bool rayIntersectsGround) {
	vec3 uvw0, uvw1;
	float lerp;
	lerpScatteringCoords(atmos, textureSize(singleRScatTex, 0),
		ray, mu_s, nu, rayIntersectsGround, uvw0, uvw1, lerp);

	vec3 m0 = rgb(texture(singleMScatTex, uvw0));
	vec3 m1 = rgb(texture(singleMScatTex, uvw1));

	vec3 r0 = rgb(texture(singleRScatTex, uvw0));
	vec3 r1 = rgb(texture(singleRScatTex, uvw1));

	vec3 m = phase(nu, atmos.mieG) * mix(m0, m1, lerp);
	vec3 r = phaseRayleigh(nu) * mix(r0, r1, lerp);
	return r + m;
}

vec3 getScattering(IN(Atmosphere) atmos,
		IN(sampler3D) scatTex, IN(ARay) ray, float mu_s, float nu,
		bool rayIntersectsGround) {
	vec3 uvw0, uvw1;
	float lerp;
	lerpScatteringCoords(atmos, textureSize(scatTex, 0),
		ray, mu_s, nu, rayIntersectsGround, uvw0, uvw1, lerp);

	vec3 s0 = rgb(texture(scatTex, uvw0));
	vec3 s1 = rgb(texture(scatTex, uvw1));
	return mix(s0, s1, lerp);
}

// Possibly multi scattering.
vec3 getScattering(IN(Atmosphere) atmos,
		IN(sampler3D) singleRScatTex, IN(sampler3D) singleMScatTex,
		IN(sampler3D) multiScatTex, IN(ARay) ray, float mu_s, float nu,
		bool rayIntersectsGround, uint scatOrder) {
	if(scatOrder == 1) {
		return getScattering(atmos, singleRScatTex, singleMScatTex,
			ray, mu_s, nu, rayIntersectsGround);
	}

	return getScattering(atmos, multiScatTex, ray, mu_s, nu,
		rayIntersectsGround);
}

// TODO: confusing naming
vec3 getCombinedScattering(IN(Atmosphere) atmos,
		IN(sampler3D) multiScatTex, IN(sampler3D) singleMScatTex,
		IN(ARay) ray, float mu_s, float nu, bool rayIntersectsGround,
		OUT(vec3) singleMieScattering) {
	vec3 uvw0, uvw1;
	float lerp;
	lerpScatteringCoords(atmos, textureSize(multiScatTex, 0),
		ray, mu_s, nu, rayIntersectsGround, uvw0, uvw1, lerp);

	singleMieScattering = mix(
		rgb(texture(singleMScatTex, uvw0)),
		rgb(texture(singleMScatTex, uvw1)),
		lerp);
	return mix(
		rgb(texture(multiScatTex, uvw0)),
		rgb(texture(multiScatTex, uvw1)),
		lerp);
}

// ground irradiance
// most naive mapping between ray and tex unit
vec3 computeIndirectIrradiance(IN(Atmosphere) atmos,
		IN(sampler3D) singleMScatTex, IN(sampler3D) singleRScatTex,
		IN(sampler3D) multiScatTex, IN(ARay) toSun, uint scatOrder) {
	assert(toSun.height >= atmos.bottom && toSun.height <= atmos.top);
	assert(toSun.mu >= -1.0 && toSun.mu <= 1.0);
	assert(scatOrder >= 1);

	const uint sampleCount = 32u;
	const float dphi = pi / sampleCount;
	const float dtheta = pi / sampleCount;

	vec3 result = vec3(0.f, 0.f, 0.f);
	vec3 omega_s = vec3(sqrt(1.f - toSun.mu * toSun.mu), 0.f, toSun.mu);

	// Integrate over hemisphere
	for(uint j = 0u; j < sampleCount / 2; ++j) {
		float theta = (j + 0.5) * dtheta; // elevation
		float cos_theta = cos(theta);
		float sin_theta = sin(theta);
		ARay inRay = {toSun.height, cos_theta};

		for(uint i = 0u; i < 2 * sampleCount; ++i) {
			float phi = (i + 0.5) * dphi; // azimuth
			vec3 omega = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
			float domega = dtheta * dphi * sin_theta; // solid angle

			float nu = dot(omega, omega_s);
			result += cos_theta * domega * getScattering(atmos,
				singleRScatTex, singleMScatTex, multiScatTex, inRay,
				toSun.mu, nu, false, scatOrder);
		}
	}
	return result;
}

vec3 computeDirectIrradiance(IN(Atmosphere) atmos,
		IN(sampler2D) transTex, IN(ARay) toSun) {
	assert(toSun.height >= atmos.bottom && toSun.height <= atmos.top);
	assert(toSun.mu >= -1.0 && toSun.mu <= 1.0);

	// Approximate average of the cosine factor mu_s over the visible fraction of
	// the Sun disc.
	float alpha_s = atmos.sunAngularRadius;
	float avgCosFac =
		toSun.mu < -alpha_s ? 0.0 :
		(toSun.mu > alpha_s ? toSun.mu :
		(toSun.mu + alpha_s) * (toSun.mu + alpha_s) / (4.0 * alpha_s));

	vec3 sun = avgCosFac * rgb(atmos.solarIrradiance);
	return sun * transmittanceToTop(atmos, transTex, toSun);
}

ARay rayFromIrradianceTexUnit(IN(Atmosphere) atmos, vec2 unit) {
	assert(unit.x >= 0.0 && unit.x <= 1.0);
	assert(unit.y >= 0.0 && unit.y <= 1.0);

	ARay res;
  	res.height = atmos.bottom + unit.y * (atmos.top - atmos.bottom);
  	res.mu = clamp(2.f * unit.x - 1.f, -1.f, 1.f);
	return res;
}

vec2 irradianceTexUnitFromRay(IN(Atmosphere) atmos, IN(ARay) toSun) {
	assert(toSun.height >= atmos.bottom && toSun.height <= atmos.top);
	assert(toSun.mu >= -1.f && toSun.mu <= 1.f);
	float x_r = (toSun.height - atmos.bottom) / (atmos.top - atmos.bottom);
	float x_mu_s = 0.5 * toSun.mu + 0.5;
	return vec2(x_mu_s, x_r);
}

vec3 getIrradiance(IN(Atmosphere) atmos, IN(sampler2D) irradianceTex,
		IN(ARay) toSun) {
	ivec2 texSize = textureSize(irradianceTex, 0);
	vec2 uv = uvFromUnitRange(irradianceTexUnitFromRay(atmos, toSun), texSize);
	return rgb(texture(irradianceTex, uv));
}

// === multi-scattering ===
// Returns the combined rayleigh and mie scattering this is scattered
// back along 'ray' (i.e. in direction of -ray.mu) from all incoming
// directions, using the given previously computed scattering LUTs.
vec3 computeIncomingScattering(IN(Atmosphere) atmos, IN(sampler2D) transTex,
		IN(sampler3D) singleRScatTex, IN(sampler3D) singleMScatTex,
		IN(sampler3D) multiScatTex, IN(sampler2D) groundTex,
		IN(ARay) ray, float mu_s, float nu, uint scatOrder) {

	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.0 && ray.mu <= 1.0);
	assert(mu_s >= -1.0 && mu_s <= 1.0);
	assert(nu >= -1.0 && nu <= 1.0);
	assert(scatOrder >= 2);

	// Compute (any, examplary) unit direction vectors for the zenith, the view
	// direction omega and and the sun direction omega_s, such that the cosine of
	// the view-zenith angle is mu, the cosine of the sun-zenith angle is mu_s,
	// and the cosine of the view-sun angle is nu.
	// The goal is to simplify computations below.
	vec3 zenith = vec3(0.f, 0.f, 1.f);
	vec3 omega = vec3(sqrt(1.f - ray.mu * ray.mu), 0.f, ray.mu);
	float sunX = omega.x == 0.0 ? 0.0 : (nu - ray.mu * mu_s) / omega.x;
	float sunY = sqrt(max(1.0 - sunX * sunX - mu_s * mu_s, 0.0));
	vec3 omega_s = vec3(sunX, sunY, mu_s);

	const uint sampleCount = 16u;
	const float dphi = pi / sampleCount;
	const float dtheta = pi / sampleCount;
	vec3 sum = vec3(0.f, 0.f, 0.f);

	// Nested loops for the integral over all the incident directions omega_i.
	for(uint l = 0u; l < sampleCount; ++l) {
		float theta = (l + 0.5) * dtheta; // elevation
		float cos_theta = cos(theta);
		float sin_theta = sin(theta);

		ARay inRay = {ray.height, cos_theta};
		bool inIntersectsGround = intersectsGround(atmos, inRay);

		// The distance and transmittance to the ground only depend on theta, so we
		// can compute them in the outer loop for efficiency.
		float distToGround;
		vec3 transToGround;
		vec3 groundAlbedo;
		if(inIntersectsGround) {
			distToGround = distanceToBottom(atmos, inRay);
			transToGround = getTransmittance(atmos, transTex, inRay, distToGround, true);

			// we simply assume uniform ground albedo. Good enough for most purposes,
			// but we couldn't reconstruct the real position where this ray hits
			// the ground anyways.
			groundAlbedo = rgb(atmos.groundAlbedo);
		}

		for(uint m = 0u; m < 2 * sampleCount; ++m) {
			float phi = (m + 0.5) * dphi; // azimuth

			// omega_i: direction (outgoing) of solid we are integrating
			// the incoming radiance over.
			vec3 omega_i = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
			float domega_i = dtheta * dphi * sin_theta; // solid angle

			// The radiance L_i arriving from direction omega_i after n-1 bounces is
			// the sum of a term given by the precomputed scattering texture for the
			// (n-1)-th order:
			float nu1 = dot(omega_s, omega_i);
			vec3 inRadiance = getScattering(atmos,
				singleRScatTex, singleMScatTex, multiScatTex,
				inRay, mu_s, nu1, inIntersectsGround, scatOrder - 1);

			// and of the contribution from the light paths with n-1 bounces and whose
			// last bounce is on the ground. This contribution is the product of the
			// transmittance to the ground, the ground albedo, the ground BRDF, and
			// the irradiance received on the ground after n-2 bounces.
			if(inIntersectsGround) {
				vec3 groundNormal = normalize(ray.height * zenith + distToGround * omega_i);
				ARay toSun = {atmos.bottom, dot(groundNormal, omega_s)};
				vec3 groundIrradiance = getIrradiance(atmos, groundTex, toSun);
				inRadiance += transToGround * groundAlbedo * (1.0 / pi) * groundIrradiance;
			}

			// The radiance finally scattered from direction omega_i towards direction
			// -omega is the product of the incident radiance, the scattering
			// coefficient, and the phase function for directions omega and omega_i
			// (all this summed over all particle types, i.e. Rayleigh and Mie).
			float nu2 = dot(omega, omega_i);
			float rDens = density(atmos.rayleighDensity, ray.height - atmos.bottom);
			float mDens = density(atmos.mieDensity, ray.height - atmos.bottom);
			sum += inRadiance * domega_i * (
				rgb(atmos.rayleighScattering) * rDens * phaseRayleigh(nu2) +
				rgb(atmos.mieScattering) * mDens * phase(atmos.mieG, nu2));
		}
	}

	return sum;
}

vec3 computeMultipleScattering(IN(Atmosphere) atmos, IN(sampler2D) transTex,
		IN(sampler3D) inScatTex, IN(ARay) ray, float mu_s, float nu,
		bool rayIntersectsGround) {
	assert(ray.height >= atmos.bottom && ray.height <= atmos.top);
	assert(ray.mu >= -1.0 && ray.mu <= 1.0);
	assert(mu_s >= -1.0 && mu_s <= 1.0);
	assert(nu >= -1.0 && nu <= 1.0);

	const uint sampleCount = 50u;
	float dt = distanceToNearestBoundary(atmos, ray, rayIntersectsGround) / sampleCount;

	vec3 sum = vec3(0.f, 0.f, 0.f);
	for(uint i = 0u; i <= sampleCount; ++i) {
		float t_i = i * dt;
		float w_i = (i == 0 || i == sampleCount) ? 0.5 : 1.0;

		// The r, mu and mu_s parameters at the current integration point (see the
		// single scattering section for a detailed explanation).
		float rr = t_i * t_i + 2.0 * ray.height * ray.mu * t_i + ray.height * ray.height;
		float r_i = clamp(sqrt(rr), atmos.bottom, atmos.top);
		float mu_i = clamp((ray.height * ray.mu + t_i) / r_i, -1.f, 1.f);
		float mu_s_i = clamp((ray.height * mu_s + t_i * nu) / r_i, -1.f, 1.f);

		// The Rayleigh and Mie multiple scattering at the current sample point.
		ARay ray_i = {r_i, mu_i};
		vec3 scat_i = getScattering(atmos, inScatTex, ray_i, mu_s_i, nu, rayIntersectsGround) / 100000.f;
		vec3 trans_i = getTransmittance(atmos, transTex, ray, t_i, rayIntersectsGround);
		// Sample weight (from the trapezoidal rule).
		sum += w_i * dt * trans_i * scat_i;
	}

	return sum;
}

// TODO: wip
vec3 getSkyRadianceToPoint(
		IN(Atmosphere) atmosphere,
		IN(sampler2D) transmittance_texture,
		IN(sampler3D) scattering_texture,
		IN(sampler3D) single_mie_scattering_texture,
		ARay ray, float mu_s, float nu, bool rayIntersectsGround, float d,
		OUT(vec3) transmittance) {
	float r = ray.height;
	float mu = ray.mu;
	if(!rayIntersectsGround){
		float mu_horiz = -sqrt(max(1.0 - (atmosphere.bottom / r) * (atmosphere.bottom / r), 0.0));
		mu = max(mu, mu_horiz + 0.004f);
		ray.mu = mu;
	}

	transmittance = getTransmittance(atmosphere, transmittance_texture,
			ray, d, rayIntersectsGround);

	vec3 single_mie_scattering;
	vec3 scattering = getCombinedScattering(
			atmosphere, scattering_texture, single_mie_scattering_texture,
			ray, mu_s, nu, rayIntersectsGround,
			single_mie_scattering);

	// Compute the r, mu, mu_s and nu parameters for the second texture lookup.
	// If shadow_length is not 0 (case of light shafts), we want to ignore the
	// scattering along the last shadow_length meters of the view ray, which we
	// do by subtracting shadow_length from d (this way scattering_p is equal to
	// the S|x_s=x_0-lv term in Eq. (17) of our paper).
	// d = max(d - shadow_length, 0.f);
	d = max(d, 0.f);
	float r_p = clamp(sqrt(d * d + 2.f * r * mu * d + r * r),
			atmosphere.bottom, atmosphere.top);
	float mu_p = (r * mu + d) / r_p;
	float mu_s_p = (r * mu_s + d * nu) / r_p;

	vec3 single_mie_scattering_p;
	ARay ray1 = {r_p, mu_p};
	vec3 scattering_p = getCombinedScattering(
			atmosphere, scattering_texture, single_mie_scattering_texture,
			ray1, mu_s_p, nu, rayIntersectsGround,
			single_mie_scattering_p);

	// Combine the lookup results to get the scattering between camera and point.
	vec3 shadow_transmittance = transmittance;
	// if (shadow_length > 0.0) {
	//   // This is the T(x,x_s) term in Eq. (17) of our paper, for light shafts.
	//   shadow_transmittance = getTransmittance(atmosphere, transmittance_texture,
	//       ray, d, rayIntersectsGround);
	// }

	scattering = scattering - shadow_transmittance * scattering_p;
	single_mie_scattering =
		single_mie_scattering - shadow_transmittance * single_mie_scattering_p;
//
// 	  if(!rayIntersectsGround) {
// 		  const float EPS = 0.004;
// 		  float muHoriz = -sqrt(1.0 - (atmosphere.bottom / r) * (atmosphere.bottom / r));
//
// 		  if (abs(mu - muHoriz) < EPS)
// 		  {
//
// 			  float a = ((mu - muHoriz) + EPS) / (2.0 * EPS);
// 			  mu = muHoriz + EPS;
// 			  vec3 single_mie_scattering0;
// 			  vec3 single_mie_scattering1;
//
// 			  float r0 = clamp(sqrt(d * d + 2.f * r * mu * d + r * r),
// 					atmosphere.bottom, atmosphere.top);
//
// 			  float mu0 = clamp((r * mu + d) / r0,-1.f,1.f);
// 			  float mu_s_0 = clamp((r * mu_s + d * nu) / r0,-1.f,1.f);
//
// 			  ARay ray0 = {r0, mu0};
//
// 			  vec3 inScatter0 = getCombinedScattering(atmosphere, scattering_texture,single_mie_scattering_texture, ray, mu_s, nu, rayIntersectsGround, single_mie_scattering0);
// 			  vec3 inScatter1 = getCombinedScattering(atmosphere, scattering_texture,single_mie_scattering_texture, ray0, mu_s_0, nu, rayIntersectsGround, single_mie_scattering1);
// 			  vec3 inScatter = max(inScatter0 - shadow_transmittance * inScatter1, vec3(0.f, 0.f, 0.f));
// 			  vec3 mie_scattering = max(single_mie_scattering0 - shadow_transmittance * single_mie_scattering1, vec3(0.f, 0.f, 0.f));
// 			  scattering = inScatter;
// 			  single_mie_scattering = mie_scattering;
// 		  }
//
// 	  }

	// Hack to avoid rendering artifacts when the sun is below the horizon.
	single_mie_scattering = single_mie_scattering *
		smoothstep(float(0.0), float(0.01), mu_s);

	return scattering * phaseRayleigh(nu) + single_mie_scattering *
		phase(nu, atmosphere.mieG);
}

/*
vec3 getSkyRadianceToPoint(
    IN(Atmosphere) atmosphere,
    IN(sampler2D) transmittance_texture,
    IN(sampler3D) scattering_texture,
    IN(sampler3D) single_mie_scattering_texture,
    vec3 camera, IN(vec3) point, float shadow_length,
    IN(vec3) sun_direction, OUT(vec3) transmittance, bool rayIntersectsGround) {
  // Compute the distance to the top atmosphere boundary along the view ray,
  // assuming the viewer is in space (or NaN if the view ray does not intersect
  // the atmosphere).
  vec3 view_ray = normalize(point - camera);
  float r = length(camera);
  float rmu = dot(camera, view_ray);
  float distance_to_top_atmosphere_boundary = -rmu -
      sqrt(rmu * rmu - r * r + atmosphere.top * atmosphere.top);
  // If the viewer is in space and the view ray intersects the atmosphere, move
  // the viewer to the top atmosphere boundary (along the view ray):
  if (distance_to_top_atmosphere_boundary > 0.0) {
    camera = camera + view_ray * distance_to_top_atmosphere_boundary;
    r = atmosphere.top;
    rmu += distance_to_top_atmosphere_boundary;
  }

  // Compute the r, mu, mu_s and nu parameters for the first texture lookup.
  float mu = rmu / r;
  float mu_s = dot(camera, sun_direction) / r;
  float nu = dot(view_ray, sun_direction);
  float d = length(point - camera);

  ARay ray = {r, mu};
  // bool ray_r_mu_intersects_ground = intersectsGround(atmosphere, ray);

  transmittance = getTransmittance(atmosphere, transmittance_texture,
      ray, d, rayIntersectsGround);

  vec3 single_mie_scattering;
  vec3 scattering = getCombinedScattering(
      atmosphere, scattering_texture, single_mie_scattering_texture,
      ray, mu_s, nu, rayIntersectsGround,
      single_mie_scattering);

  // Compute the r, mu, mu_s and nu parameters for the second texture lookup.
  // If shadow_length is not 0 (case of light shafts), we want to ignore the
  // scattering along the last shadow_length meters of the view ray, which we
  // do by subtracting shadow_length from d (this way scattering_p is equal to
  // the S|x_s=x_0-lv term in Eq. (17) of our paper).
  d = max(d - shadow_length, 0.f);
  float r_p = clamp(sqrt(d * d + 2.f * r * mu * d + r * r),
		atmosphere.bottom, atmosphere.top);
  float mu_p = (r * mu + d) / r_p;
  float mu_s_p = (r * mu_s + d * nu) / r_p;

  vec3 single_mie_scattering_p;
  ARay ray1 = {r_p, mu_p};
  vec3 scattering_p = getCombinedScattering(
      atmosphere, scattering_texture, single_mie_scattering_texture,
      ray1, mu_s_p, nu, rayIntersectsGround,
      single_mie_scattering_p);

  // Combine the lookup results to get the scattering between camera and point.
  vec3 shadow_transmittance = transmittance;
  if (shadow_length > 0.0) {
    // This is the T(x,x_s) term in Eq. (17) of our paper, for light shafts.
    shadow_transmittance = getTransmittance(atmosphere, transmittance_texture,
        ray, d, rayIntersectsGround);
  }

  scattering = scattering - shadow_transmittance * scattering_p;
  single_mie_scattering =
      single_mie_scattering - shadow_transmittance * single_mie_scattering_p;

  // Hack to avoid rendering artifacts when the sun is below the horizon.
  single_mie_scattering = single_mie_scattering *
      smoothstep(float(0.0), float(0.01), mu_s);

  return scattering * phaseRayleigh(nu) + single_mie_scattering *
      phase(nu, atmosphere.mieG);
}

*/
