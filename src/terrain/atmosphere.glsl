#include "math.glsl"

const uint maxLayers = 3u;

const uint layerExp = 0u;
const uint layerLinear = 1u;
const uint layerTent = 2u;

struct AtmosphereLayer {
	vec3 data;
	uint type;

	vec3 scattering;
	float g;
	vec3 absorption;
	float _pad;
};

struct AtmosphereDesc {
	float bottom;
	float top;
	uint nLayers;
	float _pad0;
	vec3 groundAlbedo;
	float _pad1;
	AtmosphereLayer layers[maxLayers];
};

struct VolumeSample {
	vec3 inscatter;
	vec3 extinction;
};

struct IntegratedVolume {
	vec3 inscatter;
	vec3 transmittance;
};

// Returns the density of a layer at the given height (h is given as altitude
// inside the atmosphere, i.e. r - atmos.bottom).
float density(uint layerType, vec3 layerData, float h) {
	if(layerType == layerExp) {
		return clamp(layerData[0] * exp(layerData[1] * h), 0, 1);
	} else if(layerType == layerLinear) {
		return clamp(layerData[0] + layerData[1] * h, 0, 1);
	} else if(layerType == layerTent) {
		float center = layerData[0];
		float width = layerData[1];
		return clamp(1 - abs(h - center) / width, 0, 1);
	} else {
		return -1.f;
	}
}

// Returns whether the given ray intersects the ground (i.e. the bottom
// of the given atmosphere).
bool intersectsGround(AtmosphereDesc atmos, float r, float mu) {
	return mu < 0.0 && r * r * (mu * mu - 1.0) + atmos.bottom * atmos.bottom >= 0.0;
}

// Returns the distance (along the ray) to its intersection with
// the bottom of the atmosphere. Not all rays intersect with
// the atmosphere! Check 'intersectsGround' before.
// Will return undefined value if there is no intersection.
float distanceToBottom(AtmosphereDesc atmos, float r, float mu) {
	float d = r * r * (mu * mu - 1.0) + atmos.bottom * atmos.bottom;
	return max(-r * mu - sqrt(max(d, 0.f)), 0.f);
}

// Returns the distance (along the ray) to its intersection with
// the top of the atmosphere. Every ray (starting inside the atmosphere,
// as we always assume) has it.
float distanceToTop(AtmosphereDesc atmos, float r, float mu) { 
	float d = r * r * (mu * mu - 1.0) + atmos.top * atmos.top;
	return max(-r * mu + sqrt(max(d, 0.f)), 0.f);
}

float distanceToNearestBoundary(AtmosphereDesc atmos, float r, float mu,
		bool rayIntersectsGround) {
	return rayIntersectsGround ?
		distanceToBottom(atmos, r, mu) :
		distanceToTop(atmos, r, mu);
}

float distanceToNearestBoundary(AtmosphereDesc atmos, float r, float mu) {
	bool rayIntersectsGround = intersectsGround(atmos, r, mu);
	return rayIntersectsGround ?
		distanceToBottom(atmos, r, mu) :
		distanceToTop(atmos, r, mu);
}

// basically rescales coordinates over texture.
vec2 uvFromUnitRange(vec2 range, ivec2 size) {
	return 0.5f / size + range * (1.f - 1.f / size);
}

vec2 transTexUnitFromRay(in AtmosphereDesc atmos, float r, float mu) {
	// Distance to top atmosphere boundary for a horizontal ray at ground level.
	float H = sqrt(atmos.top * atmos.top - atmos.bottom * atmos.bottom);
	// Distance to the horizon.
	float rho = sqrt(max(r * r - atmos.bottom * atmos.bottom, 0.f));
	// Distance to the top atmosphere boundary for the ray (r,mu), and its minimum
	// and maximum values over all mu - obtained for (r,1) and (r,mu_horizon).
	float d = distanceToTop(atmos, r, mu);
	float d_min = atmos.top - r;
	float d_max = rho + H;
	float x_mu = (d - d_min) / (d_max - d_min);
	float x_r = rho / H;
	return vec2(x_mu, x_r);
}

vec3 transmittanceToTop(AtmosphereDesc atmos, sampler2D transTex, float r, float mu) {
	vec2 unit = transTexUnitFromRay(atmos, r, mu);
	vec2 uv = uvFromUnitRange(unit, textureSize(transTex, 0));
	return texture(transTex, uv).rgb;
}

vec3 transmittanceToSun(AtmosphereDesc atmos, sampler2D transTex, float r, float muS) {
	const float sunAngularRadius = 0.004;
	vec3 trans = transmittanceToTop(atmos, transTex, r, muS);

	// fraction of the sun above the horizon
	float st = atmos.bottom / r;
	float ct = -sqrt(max(0.f, 1.f - st * st));
	float above = smoothstep(
		-st * sunAngularRadius,
		st * sunAngularRadius, muS - ct);
	return above * trans;
}

// cornette-shanks phase function approximation
float phaseCS(float nu, float g) {
	const float gg = g * g;
	const float fac = 3 / (8 * pi);
	const float num = (1 - gg) * (1 + nu * nu);
	const float denom = (2 + gg) * pow(1 + gg - 2 * g * nu, 1.5);
	return fac * num / denom;
}

// henyey-greenstein phase function approximation
float phaseHG(float nu, float g) {
	float gg = g * g;
	const float fac = 0.07957747154;
	return fac * ((1 - gg)) / (pow(1 + gg - 2 * g * nu, 1.5f));
}

VolumeSample sampleAtmosphere(AtmosphereDesc atmos, float r, float mu, float muS, 
		float nu, sampler2D transmittanceLUT, float shadow, vec3 solarIrradiance) {

	VolumeSample as;
	as.inscatter = vec3(0.0);
	as.extinction = vec3(0.0);

	float h = r - atmos.bottom;
	vec3 scattering = vec3(0.0);
	for(uint l = 0u; l < atmos.nLayers; ++l) {

		AtmosphereLayer layer = atmos.layers[l];
		float d = density(layer.type, layer.data, h);
		as.extinction += d * (layer.absorption + layer.scattering);

		float phase;
		if(layer.g == 0.0) {
			// rayleigh
			const float fac = 0.05968310365; // 3 / (16 * pi) for normalization
			phase = fac * (1 + nu * nu);
		} else {
			phase = phaseHG(nu, layer.g);
		}

		scattering += d * layer.scattering * phase;
	}

	vec3 trans = transmittanceToSun(atmos, transmittanceLUT, r, muS);
	as.inscatter += shadow * scattering * trans * solarIrradiance;

	// TODO: multiscatter term

	return as;
}

void integrateStep(inout vec3 scatterAccum, inout vec3 transmittance, float stepLength,
		vec3 scattering, vec3 extinction) {
	vec3 tstep = exp(-stepLength * extinction);

	// Naive solution. Or other way around. Both not optimal..
	// transmittance *= tstep;
	// scatterAccum += stepLength * transmittance * scattering;

	// Better volumetric integration after S. Hillaire
	if(dot(extinction, extinction) > 1e-10) {
		vec3 inscatter = scattering * (1 - tstep) / extinction;
		scatterAccum += transmittance * inscatter;
		transmittance *= tstep;
	}
}

void integrateStep(inout IntegratedVolume iv, float stepLength, VolumeSample vs) {
	integrateStep(iv.inscatter, iv.transmittance, stepLength, vs.inscatter, vs.extinction);
}
