#version 450

layout(location = 0) in vec3 inPos;

layout(set = 0, binding = 0, row_major) uniform Matrices {
	mat4 matrix; // view and projection
} view;

layout(set = 1, binding = 0, row_major) uniform Model {
	mat4 matrix; // model matrix
} model;

void main() {
	gl_Position = view.matrix * model.matrix * vec4(inPos, 1.0);
	gl_Position.y = -gl_Position.y;
}

