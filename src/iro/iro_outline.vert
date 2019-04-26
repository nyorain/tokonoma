#version 450
#extension GL_GOOGLE_include_directive : enable

#include "hex.glsl"

layout(location = 0) in vec2 inPos;

layout(set = 0, binding = 0, row_major) uniform Transform {
	mat4 transform;
} ubo;

void main() {
	gl_Position = ubo.transform * vec4(hexPoint(inPos, 1.f, gl_VertexIndex), 0.0, 1.0);
}
