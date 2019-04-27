#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 outPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj; // view and pojection
} scene;

layout(set = 2, binding = 0, row_major) uniform Model {
	mat4 matrix; // model matrix
	mat4 normal; // normal matrix (transpose(inverse(matrix))); effectively 3x3
} model;

void main() {
	outNormal = mat3(model.normal) * inNormal;
	outUV = inUV;

	vec4 m = model.matrix * vec4(inPos, 1.0);
	outPos = m.xyz / m.w;

	gl_Position = scene.proj * m;
	gl_Position.y = -gl_Position.y;
}
