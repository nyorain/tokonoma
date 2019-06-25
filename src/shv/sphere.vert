#version 450

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 outNormal;

layout(set = 0, binding = 0, row_major) uniform Camera {
	mat4 proj;
} camera;

void main() {
	outNormal = inPos; // sphere is centered in origin so normal is position
	gl_Position = camera.proj * vec4(inPos, 1.0);
	gl_Position.y = -gl_Position.y;
}
