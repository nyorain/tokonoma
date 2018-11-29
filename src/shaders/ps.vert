#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in float inalpha;

layout(location = 0) out float outalpha;

layout(set = 0, binding = 0) uniform UBO {
	float _alpha;
	float pointSize;
} ubo;

void main() {
	gl_Position = vec4(inPos, 0.0, 1.0);
	gl_PointSize = ubo.pointSize;
	outalpha = inalpha;
}

