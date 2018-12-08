#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

layout(location = 0) in float inDistance;
layout(location = 1) in vec2 inPointA;
layout(location = 2) in vec2 inPointB;
layout(location = 3) in vec2 inPos;

layout(location = 0) out vec2 outShadow;

layout(set = 1, binding = 0) uniform Light {
	vec4 _color;
	vec2 pos;
	float _r;
	float _strength;
	float bounds;
} light;

void main() {
	vec2 lightDir = inPos - light.pos;

	Line seg = {inPointA, inPointB - inPointA};
	Line ray = {inPos, -lightDir}; // from pixel to light

	vec2 f = intersectionFacs(seg, ray);

	// TODO
	if(inDistance > 0.f) {
		// 0.1: offset
		outShadow.r = 0.1 + length(f.y * lightDir);
		// outShadow.g = 0.f;
	} else {
		// outShadow.g = fac * length(f.y * lightDir);
		outShadow.r = -0.1 - length(f.y * lightDir);
		// outShadow.r = 0.f;
	}

	// outShadow = fac * inDistance;
}
