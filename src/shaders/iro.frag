#version 450

layout(location = 0) in vec3 inColor;
layout(location = 0) out vec4 outColor;

void main() {
	outColor = vec4(vec3(0.01) + inColor, 1.0);
}

