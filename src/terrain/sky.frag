#version 450

#extension GL_GOOGLE_include_directive : require
#include "scatter.glsl"

layout(location = 0) in vec3 pos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, row_major) uniform Camera {
	mat4 _VP;
	vec3 pos;
	float time; // time of day, 0 - 1
} ubo;

const vec3 sunColor = 10 * vec3(1, 1, 1);

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

	float te = intersectRaySphere(cpos, dir, vec3(0.0), planetRadius);
	float ta = intersectRaySphere(cpos, dir, vec3(0.0), atmosphereRadius);

	// viewing down on earth
	if(te > 0.0 || ta < 0.0) {
		// fragColor = vec4(vec3(0.0), 1.0);
		// return;
		ta = te;
	}

	float totalOD;
	vec3 rayEnd = cpos + ta * dir;
	vec3 inscatter = sampleRay(cpos, rayEnd, sunDir, totalOD);

	// outscatter
	vec3 color = inscatter * phaseRayleigh(dot(dir, sunDir)) * sunColor;
	// color += 0.01 * clamp(dot(sunDir, dir), 0, 1) * sunColor;

	// fragColor = vec4(0.0001 * vec3(ta), 1.0);
	fragColor = vec4(1.0 - exp(-color), 1.0);
	// fragColor = vec4(color, 1.0);
	// fragColor = vec4(0.5 + 0.5 * dir, 1.0);
	// fragColor = vec4(0.5f + 0.5 * pos, 1.0);
	// fragColor = vec4(vec3(0.00001 * totalOD), 1.0);

	// visualize sun position
	if(dot(-sunDir, dir) > 0.995) {
		fragColor = mix(fragColor, vec4(0.1, 0.1, 0.0, 1.0), 0.5);
		return;
	}
}
