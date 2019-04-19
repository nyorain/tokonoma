#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;

layout(set = 2, binding = 0, row_major) uniform Model {
	mat4 matrix; // model matrix
} model;

layout(set = 3, binding = 0, row_major) uniform Light {
	mat4 proj; // view and projection (of light)
} light;

layout(location = 0) out vec2 outUV;

void main() {
	gl_Position = light.proj * model.matrix * vec4(inPos, 1.0);
	gl_Position.y = -gl_Position.y;
	outUV = inUV;
}

