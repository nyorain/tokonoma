#version 450

layout(location = 0) in vec2 pos;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

void main() {
	gl_Position = ubo.transform * vec4(pos, 0, 1);
}

