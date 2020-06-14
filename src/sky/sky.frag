#version 450

#extension GL_GOOGLE_include_directive : require
#include "color.glsl"

layout(location = 0) in vec3 inCoords;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
	vec3 toSun;
	float _1;
	vec3 sunColor;
	float exposure;
	vec3 config7;
	float cosSunSize;
	vec3 config2;
	float roughness;
	vec3 rad;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D tableF; // vec3
layout(set = 0, binding = 2) uniform sampler2D tableG; // vec3
layout(set = 0, binding = 3) uniform sampler2D tableFH_H; // (vec3(FH), H)

float mapGamma(float g) {
	return sqrt(0.5f * (1.0f - g));
}

float random(vec2 v) {
	float a = 43758.5453;
	float b = 12.9898;
	float c = 78.233;
	float dt = dot(v, vec2(b, c));
	float sn = mod(dt, 3.14);
	return fract(sin(sn) * a);
}

vec3 evalSky(vec3 dir) {
	float r = ubo.roughness;
	float cosTheta = dir.y;
	float cosGamma = dot(ubo.toSun, dir);

	float t = 0.5f * (cosTheta + 1);
	float g = mapGamma(cosGamma);

	vec3 F = texture(tableF, vec2(t, r)).rgb;
	vec3 G = texture(tableG, vec2(g, r)).rgb;

	vec4 FH_H = texture(tableFH_H, vec2(t, r));
	vec3 H = FH_H.a * ubo.config7;
	vec3 FH = FH_H.rgb * ubo.config7;

	H += ubo.config2 - 1.0;
	FH += F * (ubo.config2 - 1.0);

	vec3 XYZ = (1.0 - F) * (1.0 + G) + H - FH;
	XYZ = max(ubo.rad * XYZ, 0.0);

	return XYZtoRGB(XYZ);
}

void main() {
	vec3 dir = inCoords;
	dir = normalize(dir);

	outColor = vec4(evalSky(dir), 1.0);

	// bad sun rendering...
	// TODO: somehow use limb darkening and other data from
	//   hosek model on cpu. Maybe create small texture for sun?
	if(ubo.roughness == 0.0) {
		float cosTheta = max(dot(ubo.toSun, dir), 0.0);
		float cosSunSize = ubo.cosSunSize;
		float diff = 1.0 - cosSunSize;
		float eps = 0.1 * diff;
		float sunStrength = smoothstep(cosSunSize - eps, cosSunSize + eps, cosTheta);
		sunStrength *= step(0.0, dir.y); // don't show sun below horizon
		outColor.rgb += sunStrength * ubo.sunColor;
	}

	outColor.rgb *= ubo.exposure;
	outColor.rgb = 1.0 - exp(-outColor.rgb);

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
}

