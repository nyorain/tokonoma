#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outPos;
layout(location = 1) out vec3 outNormal;

layout(set = 0, binding = 0, row_major) uniform Camera {
	mat4 proj;
} camera;

void main() {
	outPos = inPos;
	outNormal = inNormal;

	gl_Position = camera.proj * vec4(inPos, 1.0);
	gl_Position.y = -gl_Position.y;
}

