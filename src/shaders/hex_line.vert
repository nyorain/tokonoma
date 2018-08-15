#version 450
#extension GL_GOOGLE_include_directive : enable

#include "hex.glsl"

layout(location = 0) out vec4 out_color;

layout(row_major, binding = 0) uniform Transform {
	mat4 transform;
	uvec2 size;
	vec2 off;
	float radius;
};

void main() {
	out_color = vec4(0, 0, 0, 1);
	gl_Position = transform * vec4(instanceHexPoint(off, size.x, radius), 0, 1);
}
