#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec3 outPos;
layout(location = 1) out vec3 outNormal;

layout(set = 0, binding = 0, row_major) uniform Matrices {
// layout(set = 0, binding = 0) uniform Matrices {
	mat4 proj;
	mat4 model;
} ubo;

/* TODO
layout(set = 0, binding = 1, row_major) uniform Model {
	mat4 matrix; // model matrix
	mat4 normal; // normal matrix (transpose(inverse(matrix))); effectively 3x3
} model;
*/

void main() {
	// TODO: performance
	outNormal = mat3(transpose(inverse(ubo.model))) * inNormal;

	// transform
	vec4 model = ubo.model * vec4(inPos, 1.0);
	outPos = model.xyz;
	gl_Position = ubo.proj * model;
	gl_Position.y = -gl_Position.y;
}
