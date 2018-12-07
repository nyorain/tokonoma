#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inPointA;
layout(location = 2) in vec2 inPointB;
layout(location = 3) in float inOpacity;
layout(location = 4) in vec2 inValue;

// TODO
layout(location = 5) in float inDistFac;

layout(location = 0) out vec2 outColor;

layout(set = 1, binding = 0) uniform Light {
	vec4 _color; // unused
	vec2 position;
	float radius;
} light;

Circle lightCircle() {
	return Circle(light.position, light.radius);
}

void main() {
	// = shadow =
	// TODO: benchmark if branching here really makes sense
	if(inValue.x < 0) {
		outColor.g = shadowValue(lightCircle(), inPos, inPointA, inPointB);
	} else {
		outColor.g = interpolate(inValue);
	}

	// TODO: sss
	const float fac = 1.f;
	vec2 lightDir = inPos - light.position;

	Line seg = {inPointA, inPointB - inPointA};
	Line ray = {inPos, -lightDir}; // from pixel to light

	vec2 f = intersectionFacs(seg, ray);
	outColor.r = outColor.g * fac * inDistFac * length(f.y * lightDir);

	//
	outColor *= inOpacity;
}
