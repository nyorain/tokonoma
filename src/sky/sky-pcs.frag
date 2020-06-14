#version 450

#extension GL_GOOGLE_include_directive : require

// definitely supposed to mean preCosCat, btw
#include "precoscat.hpp"
#include "color.glsl"

layout(location = 0) in vec3 inCoords;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform AtmosUBO {
	mat4 _;
	Atmosphere atmos;
	vec3 sunDir;
	uint scatNuSize;
	vec3 viewPos;
};

layout(set = 0, binding = 1) uniform sampler2D transTex;
layout(set = 0, binding = 2) uniform sampler3D scatRayleighTex;
layout(set = 0, binding = 3) uniform sampler3D scatMieTex;

// only used for dithering. Bad function.
float random(vec2 v) {
	float a = 43758.5453;
	float b = 12.9898;
	float c = 78.233;
	float dt = dot(v, vec2(b, c));
	float sn = mod(dt, 3.14);
	return fract(sin(sn) * a);
}

void main() {
	vec3 startPos = viewPos;
	vec3 viewDir = normalize(inCoords);

	float r = length(startPos);
	float rmu = dot(startPos, viewDir);
	float distToTop = -rmu - sqrt(rmu * rmu - r * r + atmos.top * atmos.top);
	if(distToTop > 0.0) {
		startPos += distToTop * viewDir;
		r = atmos.top;
		rmu += distToTop;
	} else if(r > atmos.top) {
		outColor = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}

	float mu = rmu / r;
	ARay ray = {r, mu};

	float mu_s = dot(startPos, sunDir) / r;
	float nu = dot(viewDir, sunDir);
	bool rayIntersectsGround = intersectsGround(atmos, ray);

	// TODO: this is only here so the texture isn't optimized out and we it 
	// in the debug viewer. Can be removed.
	vec3 trans = texture(transTex, vec2(0.0)).rgb;

	vec3 scat;
	const bool lookup = true;
	if(lookup) {
		// lookup from texture
		scat = getScattering(atmos, scatMieTex, scatRayleighTex,
			ray, mu_s, nu, rayIntersectsGround, scatNuSize) + 0.0 * trans;
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

	float exposure = 1.f;
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

