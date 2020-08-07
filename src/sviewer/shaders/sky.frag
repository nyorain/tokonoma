#version 450
#extension GL_GOOGLE_include_directive : require

#include "../spec.glsl"
#include "math.glsl"
#include "color.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 fragColor;

// henyey-greenstein
float phase(float nu, float g) {
	float gg = g * g;
	const float fac = 0.07957747154;
	return fac * ((1 - gg)) / (pow(1 + gg - 2 * g * nu, 1.5f));
}
	
float random(vec2 v) {
	float a = 43758.5453;
	float b = 12.9898;
	float c = 78.233;
	float dt = dot(v, vec2(b, c));
	float sn = mod(dt, 3.14);
	return fract(sin(sn) * a);
}

float linearStep(float low, float high, float t) {
	return clamp((t - low) / (high - low), 0, 1);
}

vec4 sunColors[] = {
	{0.2, 0.1, 0.1, 0.0},
	{0.4, 0.2, 0.11, 0.05},
	{0.9, 0.85, 0.7, 0.2},
	{0.9, 0.85, 0.7, 0.3},
	{0.4, 0.2, 0.11, 0.45},
	{0.3, 0.1, 0.1, 0.5},
	{0.0, 0.0, 0.0, 0.6},
	{0.0, 0.0, 0.0, 0.9},
	{0.2, 0.1, 0.1, 1.0},
};

vec3 sunColor(float t) {
	for(uint i = 1u; i < sunColors.length(); ++i) {
		vec4 color = sunColors[i];
		if(t < color.a) {
			vec4 prev = sunColors[i - 1];
			float fac = smoothstep(prev.a, color.a, t);
			return mix(prev.rgb, color.rgb, fac);
		}
	}

	return vec3(1, 0, 1);
}

vec3 sunColor(vec3 dir) {
	vec3 colorHorizon = vec3(0.7, 0.4, 0.2);
	vec3 colorDown = vec3(0.0, 0.0, 0.0);
	vec3 colorUp = vec3(0.9, 0.85, 0.8);

	return smoothstep(0.0, 1.0, dir.y) * colorUp +
		smoothstep(-1.0, 0.0, dir.y) * (1 - smoothstep(0.0, 1.0, dir.y)) * colorHorizon +
		(1 - smoothstep(-1.0, 0.0, dir.y)) * colorDown;
}

vec3 ambientColor(vec3 dir) {
	vec3 colorHorizon = vec3(0.5, 0.4, 0.4);
	vec3 colorDown = vec3(0.008, 0.005, 0.01);
	vec3 colorUp = vec3(0.2, 0.3, 0.8);

	float yy1 = 1 - (1 - dir.y) * (1 - dir.y);
	return smoothstep(0.0, 1.0, yy1) * colorUp +
		smoothstep(-1.0, 0.0, dir.y) * (1 - smoothstep(0.0, 1.0, yy1)) * colorHorizon +
		(1 - smoothstep(-1.0, 0.0, dir.y)) * colorDown;
}

void main() {
	vec2 uv = 2 * inUV - 1;
	uv.y *= -1;

	const vec3 up = vec3(0, 1, 0);
	const vec3 dir = normalize(ubo.camDir);
	const vec3 x = normalize(cross(dir, up));
	const vec3 y = cross(x, dir);

	const float maxy = tan(ubo.fov / 2);
	uv *= vec2(maxy * ubo.aspect, maxy);

	const vec3 rayStart = ubo.camPos;
	const vec3 rayDir = normalize(dir + uv.x * x + uv.y * y);

	const float dayTimeN = fract(0.01 * ubo.effect);
	const float dayTime = 2 * pi * dayTimeN;
	const vec3 toLight = vec3(cos(dayTime), sin(dayTime), 0.0);

	const vec3 lightColor = sunColor(dayTimeN);
	const vec3 ambientColor = 0.5 * ambientColor(toLight);

	const float ldv = dot(rayDir, toLight);
	vec3 light = vec3(0.0);
	light += phase(ldv, 0.9) * lightColor * max(rayDir.y, 0.0);
	light += 2 * phase(ldv, 0.45) * ambientColor * lightColor;
	light += 4 * phase(ldv, 0.0) * ambientColor;

	light *= 0.025 + smoothstep(0, 1, rayDir.y);

	// tonemap
	light = 1 - exp(-light);

	// dithering
	float rnd1 = random(gl_FragCoord.xy + 0.17);
	float rnd2 = random(gl_FragCoord.xy + 0.85);
	float dither = 0.5 * (rnd1 + rnd2) - 0.5;
	light = toLinearCheap(toNonlinearCheap(light) + dither / 255.f);
	fragColor = vec4(light, 1.0);
}
