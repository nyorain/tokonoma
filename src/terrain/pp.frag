#version 450
#extension GL_GOOGLE_include_directive : require

#include "color.glsl"
#include "scene.glsl"
#include "constants.glsl"
#include "atmosphere.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;

layout(set = 0, binding = 2) uniform sampler2D shadowmap;
layout(set = 0, binding = 3) uniform sampler2D heightmap;

layout(set = 0, binding = 4) uniform sampler2D transmittanceLUT;

layout(set = 0, binding = 5, row_major) uniform Scene {
	UboData scene;
};

layout(set = 0, binding = 6, row_major) uniform AtmosphereUBO {
	AtmosphereDesc atmos;
	vec3 solarIrradiance;
};

vec3 skyColor(vec3 rayDir, float shadow) {
	const vec3 toLight = scene.toLight;
	const vec3 sunColor = scene.sunColor * smoothstep(-0.4, -0.1, toLight.y);

	const float nu = dot(rayDir, toLight);
	vec3 light = vec3(0.0);
	light += 1 * phaseHG(nu, 0.8) * sunColor * shadow;
	light += 2 * phaseHG(nu, 0.2) * scene.ambientColor * sunColor * shadow;
	light += 8 * phaseHG(nu, 0.0) * scene.ambientColor;

	return light;
}

IntegratedVolume computeScattering(vec3 posWS, float depth) {
	vec3 rayStart = scene.viewPos;
	vec3 rayDir = posWS - scene.viewPos;
	float dist = distance(posWS, scene.viewPos);
	rayDir /= dist;

	float r = atmos.bottom + scene.viewPos.y;
	float mu = rayDir.y;
	float nu = dot(rayDir, scene.toLight);
	float muS = scene.toLight.y;
	if(depth > 0.999) {
		// TODO: does not work when looking through atmosphere from side
		dist = distanceToNearestBoundary(atmos, r, mu);
		posWS = scene.viewPos + dist * rayDir;
	}

	// iterations for shadow
	// float stepSize = 0.02;
	float stepSize = 0.02;
	vec3 pos = rayStart;
	float shadow = 0.0;
	float t = 0.0;
	float roff = random(posWS + 0.1 * scene.time);

	IntegratedVolume iv;
	iv.inscatter = vec3(0.0);
	iv.transmittance = vec3(1.0);

	for(uint i = 0u; t < dist && i < 1024; ++i) {
		float shadow = 1.0;

		pos = rayStart + (t + roff * stepSize) * rayDir;	
		if(pos == clamp(pos, -1.5, 1.5)) {
			float height = texture(heightmap, 0.5 + 0.5 * pos.xz).r;
			float shadowHeight = texture(shadowmap, 0.5 + 0.5 * pos.xz).r;
			shadow *= 1 - smoothstep(pos.y - height - 0.01, pos.y - height, shadowHeight);
		} else {
			// stepSize *= min(10, length(pos.xz));
			// stepSize *= 1 + 10 * random(posWS + 0.1 * scene.time);
			stepSize = max(max(stepSize, 1), 0.1 * t);
		}

		float d = t + roff * stepSize;
		float r_d = clamp(sqrt(d * d + 2.f * r * mu * d + r * r), atmos.bottom, atmos.top);
		float muS_d = clamp((r * muS + d * nu) / r_d, -1.f, 1.f);
		VolumeSample vs = sampleAtmosphere(atmos, r_d, mu, muS_d, nu,
			transmittanceLUT, shadow, solarIrradiance);

		if(t + stepSize > dist) {
			stepSize = dist - t;
		}

		integrateStep(iv, stepSize, vs);
		t += stepSize;
		// stepSize = max(stepSize, 0.5 * t);
	}

	return iv;
}

void main() {
	vec3 color = texture(colorTex, inUV).rgb;

	// get world space pos
	// float depth = texture(depthTex, inUV).r;
	// vec3 posWS = reconstructWorldPos(inUV, scene.invVP, depth);

	// scattering
	// TODO: move to volumetric pass
	// IntegratedVolume iv = computeScattering(posWS, depth);
	// color = iv.transmittance * color + iv.inscatter;

	// tonemap
	float exposure = 1.0;
	color = 1 - exp(-exposure * color);

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
	outFragColor = vec4(toLinearCheap(toNonlinearCheap(color) + dither / 255.f), 1.0);
}
