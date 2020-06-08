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
layout(binding = 1) uniform sampler2D img;

void main() {
	uint cx = gl_InstanceIndex % size.x;
	uint cy = gl_InstanceIndex / size.x;
	vec2 color = texture(img, (vec2(cx, cy) + 0.5) / size).xy;

	// tonemap
	color = 1.0 - exp(-color);

	out_color = vec4(color, 0, 1);
	gl_Position = transform * vec4(instanceHexGridPoint(off, size.x, radius), 0, 1);
}

