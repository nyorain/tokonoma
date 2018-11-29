#version 450

layout(location = 0) in float inalpha;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform UBO {
	float alpha;
	float _pointSize;
} ubo;

void main() {
	outColor = vec4(1.0, 1.0, 1.0, ubo.alpha * inalpha);
}
