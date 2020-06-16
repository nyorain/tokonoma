#version 450

layout(location = 0) in vec2 inColor;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform UBO {
	float alpha;
} ubo;

void main() {
	outColor = vec4(inColor, 0.0, ubo.alpha);
}
