#version 450

#extension GL_GOOGLE_include_directive : require

// Definitely supposed to mean preCosCat, btw, i.e. just the cat 
// instead of cos(cat).
#include "precoscat.hpp"
#include "color.glsl"

layout(location = 0) in vec3 inCoords;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform AtmosUBO {
	mat4 _1;
	Atmosphere atmos;
	vec3 sunDir;
	float _2;
	vec3 viewPos;
};

layout(set = 0, binding = 1) uniform sampler2D transTex;
layout(set = 0, binding = 2) uniform sampler3D scatRayleighTex;
layout(set = 0, binding = 3) uniform sampler3D scatMieTex;
layout(set = 0, binding = 4) uniform sampler3D scatCombinedTex;
layout(set = 0, binding = 5) uniform sampler2D groundTex;
layout(set = 0, binding = 6) uniform samplerCube starMap;

layout(push_constant) uniform PCR {
	uint scatOrder;
};

// only used for dithering. Bad function.
float random(vec2 v) {
	float a = 43758.5453;
	float b = 12.9898;
	float c = 78.233;
	float dt = dot(v, vec2(b, c));
	float sn = mod(dt, 3.14);
	return fract(sin(sn) * a);
}

vec3 getIn() {
	vec3 startPos = viewPos;
	vec3 viewDir = normalize(inCoords);

	float r = length(startPos);
	float rmu = dot(startPos, viewDir);
	float distToTop = -rmu - sqrt(rmu * rmu - r * r + atmos.top * atmos.top);

	vec3 bgColor = 1e-5 * texture(starMap, viewDir.xzy).rgb;
	if(distToTop > 0.0) {
		startPos += distToTop * viewDir;
		r = atmos.top;
		rmu += distToTop;
	} else if(r > atmos.top) {
		return bgColor;
	}

	float mu = rmu / r;
	ARay ray = {r, mu};

	float mu_s = dot(startPos, sunDir) / r;
	float nu = dot(viewDir, sunDir);
	bool rayIntersectsGround = intersectsGround(atmos, ray);

	vec3 scat;
	const bool lookup = true;
	if(lookup) {
		// lookup from texture
		if(scatOrder == 1) {
			scat = getScattering(atmos, scatRayleighTex, scatMieTex,
				ray, mu_s, nu, rayIntersectsGround);
		} else {
			scat = getScattering(atmos, scatCombinedTex, scatMieTex,
				ray, mu_s, nu, rayIntersectsGround);
		}
	} else {
		// reference: just recalculate it per pixel.
		// Useful to detect texture mapping issues.
		// Note that this still uses the transmission texture (otherwise
		// it wouldn't be doable in realtime, given the number of plain
		// samples we use). The transmission texture/pre-calc is way easier
		// and simpler to debug though
		vec3 sr, sm;
		singleScattering(atmos, transTex, ray, mu_s, nu, 
			rayIntersectsGround, sr, sm);
		scat = phaseRayleigh(nu) * sr + phase(nu, atmos.mieG) * sm;
	}

	if(rayIntersectsGround) {
		float r_d = atmos.bottom;
		float d = distanceToBottom(atmos, ray);

		vec3 albedo = vec3(atmos.groundAlbedo);
		vec3 normal = normalize(startPos + d * viewDir);

		// indirect
		float mu_s_d = clamp((ray.height * mu_s + d * nu) / r_d, -1.f, 1.f);

		vec3 transToGround = getTransmittance(atmos, transTex, ray, d, true);
		ARay toSun = {r_d, mu_s_d};
		vec3 groundIrradiance = getGroundIrradiance(atmos, groundTex, toSun);
		scat += (1 / pi) * albedo * transToGround * groundIrradiance;

		// direct
		scat += albedo * vec3(atmos.solarIrradiance) * 
			transmittanceToSun(atmos, transTex, toSun) *
			max(dot(normal, sunDir), 0.0);
	} else {
		vec3 transToTop = transmittanceToTop(atmos, transTex, ray);
		scat += bgColor * transToTop;
	}

	return scat;
}

void main() {
	vec3 scat = getIn();
	
	float exposure = 4.f;
	scat = 1.0 - exp(-exposure * scat);
	outColor = vec4(clamp(scat, 0.f, 1.f), 1.0);

	// anti-banding dithering
	// important that we do this after conversion to srgb, i.e. on the
	// real, final 8-bit pixel values
	// Also important: no bias here
	// We use a triangular noise distribution for best results.
	// TODO: use blue noise instead for random samples.
	// TODO: tri distr can be achieved more efficiently with a single random
	//  sample, just search shadertoy (+ discussions on best remapping)
	float rnd1 = random(gl_FragCoord.xy + 0.17);
	float rnd2 = random(gl_FragCoord.xy + 0.85);
	float dither = 0.5 * (rnd1 + rnd2) - 0.5;
	outColor.rgb = toLinearCheap(toNonlinearCheap(outColor.rgb) + dither / 255.f);

	// debug
	// outColor = vec4(0.5 + 0.5 * mu, 0.5 + 0.5 * mu_s, 0.5 + 0.5 * nu, 1.0);
}

