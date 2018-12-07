#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

layout(location = 0) in float inDistance;
layout(location = 1) in vec2 inPointA;
layout(location = 2) in vec2 inPointB;
layout(location = 3) in vec2 inPos;

layout(location = 0) out float outShadow;

layout(set = 1, binding = 0) uniform Light {
	vec4 _color;
	vec2 pos;
	float _r;
	float _strength;
	float bounds;
} light;

void main() {
	const float fac = 1.f;
	vec2 lightDir = inPos - light.pos;

	Line seg = {inPointA, inPointB - inPointA};
	Line ray = {inPos, -lightDir}; // from pixel to light

	vec2 f = intersectionFacs(seg, ray);
	outShadow = fac * inDistance * length(f.y * lightDir);
}
