#version 450

layout(location = 0) out vec4 fragColor;
layout(push_constant) uniform PCR {
	vec4 color;
};

void main() {
	fragColor = color;
}

