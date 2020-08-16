#version 450

layout(location = 0) in vec2 inNormal;
layout(location = 0) out vec4 fragColor;

void main() {
	fragColor = vec4(vec2(inNormal), 0.0, 1.0);
}
