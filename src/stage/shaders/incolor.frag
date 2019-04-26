#version 450

layout(location = 0) in vec4 incol;
layout(location = 0) out vec4 outcol;

void main() {
	outcol = incol;
}
