#version 450

layout(location = 0) in vec3 inPos;

layout(set = 0, binding = 0, row_major) uniform Camera {
	mat4 proj;
} camera;

void main() {
	gl_Position = camera.proj * vec4(inPos, 1.0);
}
