#version 450

#extension GL_GOOGLE_include_directive : require
#include "scatter.glsl"

layout(location = 0) in vec3 pos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform Camera {
	mat4 _;
	vec3 position;
} camera;

const vec3 sunDir = normalize(vec3(0.5, 1.0, 0.1));

void main() {
	float t = intersectRaySphere(camera.position, normalize(pos),
		vec3(0.0), atmosphereRadius);
	vec3 color = sampleRay(camera.position, rayEnd, sunDir);
	fragColor = vec4(color, 1.0);
}
