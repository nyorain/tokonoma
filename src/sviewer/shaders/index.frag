#version 450
#extension GL_GOOGLE_include_directive : require

#include "../spec.glsl"
#include "noise.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 fragColor;

// index like 
// https://www.iquilezles.org/www/articles/texturerepetition/texturerepetition.htm

const vec3 colors[] = {
	{1, 0, 0},
	{1, 1, 0},
	{0, 1, 0},
	{0, 1, 1},
	{0, 0, 1},
	{1, 0, 1},
};

void main() {
	float index = 20 * gradientNoise(10 * (ubo.mpos + inUV));
	int id0 = int(mod(floor(index), colors.length()));
	int id1 = int(mod(id0 + 1, colors.length()));

	vec3 color = mix(colors[id0], colors[id1], smoothstep(0, 1, fract(index)));

	fragColor = vec4(color, 1.0);
}
