#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outNormal;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 proj;
	mat4 model;
} ubo;

void main() {
	gl_Position = ubo.proj * ubo.model * vec4(inPos, 1.0);
	outNormal = inNormal;
}
