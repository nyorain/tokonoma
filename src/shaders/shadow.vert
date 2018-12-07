#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

// all input happens per instance
layout(location = 0) in vec2 inPointA;
layout(location = 1) in vec2 inPointB;
layout(location = 2) in float inOpacity;

layout(location = 0) out vec2 outPos;
layout(location = 1) out vec2 outPointA;
layout(location = 2) out vec2 outPointB;
layout(location = 3) out float outOpacity;
layout(location = 4) out vec2 outValue;

// TODO
layout(location = 5) out float outDistFac; // for sss

layout(set = 1, binding = 0) uniform Light {
	vec4 _color;
	vec2 position;
	float radius;
	float _strength;
	float bounds; // size of shadow map in world coords
} light;

Circle lightCircle() {
	return Circle(light.position, light.radius);
}

void main() {
	Line segLine = {inPointA, inPointB - inPointA};

	// make sure outPointA is (math rotational, light pov) before outPointB
	bool swap = orientation(light.position, segLine) == OriRight;
	outPointA = swap ? inPointB : inPointA;
	outPointB = swap ? inPointA : inPointB;

	ShadowVertex vert = smoothShadowVertex(gl_VertexIndex, inPointA, inPointB,
		lightCircle(), inOpacity > 0);

	outOpacity = abs(inOpacity);
	outPos = vert.pos;
	outValue = vert.opacity;

	// TODO: sss
	outDistFac = swap ? 1.f : -1.f;

	float scale = 1 / light.bounds;
	gl_Position.xy = scale * (outPos - light.position);
	gl_Position.zw = vec2(0.f, 1.f);
}
