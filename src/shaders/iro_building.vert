#version 450
#extension GL_GOOGLE_include_directive : enable

const float cospi6 = 0.86602540378; // cos(pi/6) or sqrt(3)/2

// total height: 2 * radius (height per row due to shift only 1.5 * radius)
// total width: cospi6 * 2 * radius
// we start in the center so we can pass the "outRad" (normalized distance
// to center) to the fragment shader.
const vec2[] hexPoints = {
	{-1.f, -1.f},
	{1.f, -1.f},
	{1.f, 1.f},
	{-1.f, 1.f},
};

layout(location = 0) in vec2 inPos;
layout(location = 0) out vec2 outUV;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

void main() {
	vec2 p = hexPoints[gl_VertexIndex % 4];
	outUV = 0.5 + 0.5 * p;
	p.x *= cospi6;
	gl_Position = ubo.transform * vec4(inPos + p, 0.0, 1.0);
}

