#version 450
#extension GL_GOOGLE_include_directive : enable

#include "hex.glsl"

layout(location = 0) in vec2 inPos;
layout(location = 1) in uint inPlayer;
layout(location = 2) in float inStrength;

layout(location = 0) out vec3 outColor;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 transform;
} ubo;

const vec3 colors[] = {
	vec3(1.0, 1.0, 0.0),
	vec3(0.0, 1.0, 1.0),
};

void main() {
	gl_Position = ubo.transform * vec4(hexPoint(inPos, 1.f, gl_VertexIndex), 0.0, 1.0);
	outColor = inStrength * mix(colors[0], colors[1], float(inPlayer));
}
