#version 450
#extension GL_GOOGLE_include_directive : require

#include "constants.glsl"

layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 vel;
layout(location = 2) in float sediment;
layout(location = 3) in float water;

layout(location = 0) out vec3 color;

layout(set = 0, binding = 0) uniform sampler2D heightmap;
layout(set = 0, binding = 1, row_major) uniform Ubo {
	mat4 vp;
} ubo;

void main() {
	vec2 baseCoord = 0.5 + 0.5 * pos;
	float height = texture(heightmap, baseCoord).r;

	gl_Position = ubo.vp * vec4(pos.x, height + 0.001, pos.y, 1.0);
	gl_PointSize = 8.0;
	color = vec3(sediment / capacity, length(vel), 0.5 + 0.5 * water);
}
