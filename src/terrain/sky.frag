#version 450

#extension GL_GOOGLE_include_directive : require
#include "scatter.glsl"
#include "noise.glsl"

layout(location = 0) in vec3 pos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, row_major) uniform Camera {
	mat4 _VP;
	vec3 pos;
	float time; // time of day, 0 - 1
} ubo;

const vec3 sunColor = 10 * vec3(1, 1, 1);

vec3 atmosphere(vec3 cpos, vec3 dir, vec3 sunDir, out bool visualizeSun) {
	visualizeSun = true;
	float te = intersectRaySphere(cpos, dir, vec3(0.0), planetRadius);
	float ta = intersectRaySphere(cpos, dir, vec3(0.0), atmosphereRadius);

	// case: viewer inside atmosphere, watches towards outside of atmosphere
	float start = 0;
	float end = ta;

	// outside of atmosphere and doesn't look towards atmosphere
	if(ta < 0.f) {
		return vec3(0.0);
	}

	// if ray comes from outside, start at first intersection with atmosphere
	bool outsideA = length(cpos) > atmosphereRadius;
	if(outsideA) {
		start = ta;
	}

	if(te > 0.f) { // ray hits earth
		visualizeSun = false;
		end = te;
	} else if(outsideA && te == -1.f) {
		end = intersectRaySphereBack(cpos, dir, vec3(0.0), atmosphereRadius);
	} else if(te < 0.f) {
		end = ta;
	}

	vec3 rayStart = cpos + start * dir;
	vec3 rayEnd = cpos + end * dir;
	Inscatter inscatter = sampleRay(rayStart, rayEnd, sunDir, random(pos));

	// outscatter
	vec3 color = inscatter.rayleigh * phaseRayleigh(dot(dir, sunDir)) * sunColor;
	color += inscatter.mie * phase(dot(dir, sunDir), -0.792) * sunColor;
	return color;

}

void main() {
	vec3 sunDir = vec3(0.0);
	// sunDir.y = sin(2 * pi * (ubo.time - 0.25));
	sunDir.z = cos(2 * pi * (ubo.time - 0.25));
	sunDir.y = 1 - 4 * abs(0.5 - ubo.time);
	// sunDir.z = 0.05 * (-1 + 4 * abs(0.5 - ubo.time));
	sunDir = normalize(sunDir);

	vec3 dir = normalize(pos);
	// vec3 cpos = (planetRadius + 1) * normalize(vec3(0, 1, 0));
	vec3 cpos = ubo.pos;

	bool visualizeSun;
	vec3 color = atmosphere(cpos, dir, sunDir, visualizeSun);

	// fragColor = vec4(0.0001 * vec3(ta), 1.0);
	fragColor = vec4(1.0 - exp(-color), 1.0);
	// fragColor = vec4(color, 1.0);
	// fragColor = vec4(0.5 + 0.5 * dir, 1.0);
	// fragColor = vec4(0.5f + 0.5 * pos, 1.0);
	// fragColor = vec4(vec3(0.00001 * totalOD), 1.0);

	// visualize sun position
	if(dot(-sunDir, dir) > 0.995 && visualizeSun) {
		fragColor = mix(fragColor, vec4(0.1, 0.1, 0.0, 1.0), 0.5);
		return;
	}
}
