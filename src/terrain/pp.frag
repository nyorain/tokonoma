#version 450
#extension GL_GOOGLE_include_directive : require

#include "color.glsl"
#include "scene.glsl"
#include "constants.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;

layout(set = 0, binding = 2) uniform sampler2D shadowmap;
layout(set = 0, binding = 3) uniform sampler2D heightmap;

layout(set = 0, binding = 4, row_major) uniform Scene {
	UboData scene;
};

// henyey-greenstein
float phaseHG(float nu, float g) {
	float gg = g * g;
	const float fac = 0.07957747154;
	return fac * ((1 - gg)) / (pow(1 + gg - 2 * g * nu, 1.5f));
}

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

vec4 computeScattering(vec3 posWS) {
	vec3 rayStart = scene.viewPos;
	vec3 rayDir = posWS - scene.viewPos;
	float dist = distance(posWS, scene.viewPos);
	rayDir /= dist;

	// iterations for shadow
	// float stepSize = 0.02;
	const uint numSamples = 100;
	const float stepSize = min(dist, 2) / numSamples;
	vec3 pos = rayStart;
	float shadow = 0.0;
	float off = 0.0; //stepSize * random(posWS + 0.1 * scene.time);
	float t = 0.0;
	for(uint i = 0u; i < numSamples && t < dist; ++i) {
		pos = rayStart + (t + off) * rayDir;	
		if(pos.xz == clamp(pos.xz, -1.0, 1.0)) {
			float height = texture(heightmap, 0.5 + 0.5 * pos.xz).r;
			float shadowHeight = texture(shadowmap, 0.5 + 0.5 * pos.xz).r;
			shadow += stepSize * (smoothstep(pos.y - height - 0.01, pos.y - height, shadowHeight));
		}

		t += stepSize;
	}

	shadow = t == 0.0 ? 1.0 : 1 - shadow / t;

	float density = 0.5; // assumed constant
	float opticalDepth = density * dist;
	float transmittance = exp(-opticalDepth);

	vec3 inscattered = skyColor(rayDir, shadow) * density;
	inscattered = inscattered * (1 - transmittance) / density;

	return vec4(inscattered, transmittance);
}

void main() {
	vec3 color = texture(colorTex, inUV).rgb;

	// get world space pos
	float depth = texture(depthTex, inUV).r;
	vec3 posWS = reconstructWorldPos(inUV, scene.invVP, depth);

	// scattering
	vec4 scatter = computeScattering(posWS);
	color = scatter.a * color + scatter.rgb;

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
