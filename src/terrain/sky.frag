#version 450

#extension GL_GOOGLE_include_directive : require
#include "scatter.glsl"

layout(location = 0) in vec3 pos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform Camera {
	mat4 _;
	// vec3 position;
} camera;

const vec3 sunDir = normalize(vec3(0.5, 0.1, 0.1));
const vec3 sunColor = 15 * vec3(1, 1, 1);

void main() {
	vec3 dir = normalize(pos);
	vec3 cpos = vec3(0, planetRadius + 0.1, 0);

	// visualize sun position
	// if(dot(sunDir, dir) > 0.99) {
	// 	fragColor = vec4(1.0, 0.0, 0.0, 1.0);
	// 	return;
	// }

	float te = intersectRaySphere(cpos, dir, vec3(0.0), planetRadius);
	float ta = intersectRaySphere(cpos, dir, vec3(0.0), atmosphereRadius);

	// viewing down on earth
	if(te > 0.0 || ta < 0.0) {
		fragColor = vec4(vec3(0.0), 1.0);
		return;
	}

	float totalOD;
	vec3 rayEnd = cpos + ta * dir;
	vec3 inscatter = sampleRay(cpos, rayEnd, sunDir, totalOD);

	// outscatter
	vec3 color = inscatter * phaseRayleigh(dot(dir, sunDir)) * sunColor;

	// fragColor = vec4(0.0001 * vec3(ta), 1.0);
	fragColor = vec4(1.0 - exp(-color), 1.0);
	// fragColor = vec4(color, 1.0);
	// fragColor = vec4(0.5 + 0.5 * dir, 1.0);
	// fragColor = vec4(0.5f + 0.5 * pos, 1.0);
}
