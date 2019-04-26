#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inVel;
layout(location = 0) out vec2 outCol;

layout(set = 0, binding = 0) uniform UBO {
	float _alpha;
	float pointSize;
} ubo;

void main() {
	float green = 1.f - clamp(0.5 * length(inVel), 0.0, 1.0);
	outCol = vec2(1.0, green);
	gl_Position = vec4(inPos, 0.0, 1.0);
	gl_PointSize = ubo.pointSize;
}
