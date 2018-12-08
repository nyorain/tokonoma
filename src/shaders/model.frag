#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 0) out vec4 outCol;

void main() {
	outCol = vec4(0.6 + 0.4 * inNormal, 1.0);
}
