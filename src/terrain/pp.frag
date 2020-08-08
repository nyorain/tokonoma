#version 450
#extension GL_GOOGLE_include_directive : require

#include "color.glsl"
#include "scene.glsl"
#include "constants.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;

layout(set = 0, binding = 2, row_major) uniform Scene {
	mat4 _vp;
	vec3 viewPos;
	float _;
	vec3 toLight;
	float _2;

	mat4 invVP;
} scene;

// henyey-greenstein
float phaseHG(float nu, float g) {
	float gg = g * g;
	const float fac = 0.07957747154;
	return fac * ((1 - gg)) / (pow(1 + gg - 2 * g * nu, 1.5f));
}

vec4 computeScattering(vec3 posWS) {
	vec3 toCam = normalize(scene.viewPos - posWS);
	float nu = dot(scene.toLight, -toCam);
	float dist = distance(posWS, scene.viewPos);
	float phase0 = phaseHG(nu, 0.85);
	float phaseH = phaseHG(nu, 0.45);
	float phase1 = phaseHG(nu, 0.0);

	float density = 0.2; // assumed constant
	float opticalDepth = density * dist;
	float transmittance = exp(-opticalDepth);

	vec3 inscattered = vec3(0.0);
	inscattered += 0.5 * phase0 * lightColor * density;
	inscattered += 2.0 * phaseH * lightColor * ambientColor * density;
	inscattered += 8.0 * phase1 * ambientColor * density;

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
