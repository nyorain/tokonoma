#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

layout(location = 0) in vec2 inPointA;
layout(location = 1) in vec2 inPointB;

layout(location = 0) out float outDistance; // subsurf scattering distance
layout(location = 1) out vec2 outPointA;
layout(location = 2) out vec2 outPointB;
layout(location = 3) out vec2 outPos;

layout(set = 1, binding = 0) uniform Light {
	vec4 _color;
	vec2 pos;
	float _r;
	float _strength;
	float bounds;
} light;

void main() {
	Line segLine = {inPointA, inPointB - inPointA};
	bool front = orientation(light.pos, segLine) == OriRight; // TODO (?)
	float dfac = front ? 1.f : -1.f;

	outPointA = inPointA;
	outPointB = inPointB;

	// NOTE: approximate points in infinity
	// we do this to rely on the pipelines clipping instead of dealing
	// with that complexity ourselves. Shouldn't hurt performance.
	const float proj = 10.f;
	vec2 p = vec2(-10, -10);
	switch(gl_VertexIndex % 4) {
	case 0:
		p = inPointA;
		outDistance = 0.0;
		break;
	case 1:
		p = inPointB;
		outDistance = 0.0;
		break;
	case 2: {
		// project inPointA from light to infinity
		vec2 off = proj * (inPointA - light.pos);
		p = inPointA + off;
		outDistance = dfac * length(off);
		break;
	} case 3: {
		// project inPointB from light to infinity
		vec2 off = proj * (inPointB - light.pos);
		p = inPointB + off;
		outDistance = dfac * length(off);
		break;
	} default:
		p = vec2(-5, -5); // error
		break;
	}

	// TODO: apparently we can't interpolate distance?
	// outDistance = dfac;
	outPos = p;

	float scale = 1 / light.bounds;
	gl_Position = vec4(scale * (p - light.pos), 0.0, 1.0);
}
