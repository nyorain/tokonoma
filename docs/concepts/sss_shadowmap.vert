// 1D shadow mapping vertex shader

#version 450
#extension GL_GOOGLE_include_directive : enable

#include "geometry.glsl"

// the two points defining the segment
// we get both as per instance input
layout(location = 0) in vec2 inPointA;
layout(location = 1) in vec2 inPointB;

layout(location = 0) out float outFront;

layout(set = 1, binding = 0) uniform Light {
	vec4 _color;
	vec2 position;
	float _strength;
} light;

// map the point onto [-1, 1] for our 1D shadow mapping texture
// pass the depth (distance from light) on to the frag shader
void main() {
	const float pi = 3.1415926535897932;

	// whether this is a front or back "face" (segments)
	// by convention, shapes are defined counter clockwise, therefore
	// the light is on the right of a front face
	Line segLine = {inPointA, inPointB - inPointA};
	bool front = orientation(light.position, segLine) == OriRight;
	outFront = float(front);

	vec2 diffA = inPointA - light.position;
	vec2 diffB = inPointB - light.position;

	// swap them to make sure angleA < angleB (when there is no wrapping)
	// this is the case for back faces already
	diffA = front ? diffB : diffA;
	diffB = front ? diffA : diffB;

	float angleA = atan(diffA.y, diffA.x) / pi; // between -1 and 1
	float angleB = atan(diffB.y, diffB.x) / pi; // between -1 and 1

	float wrap = float(angleA > angleB);

	// second two are in case something wrapped.
	// TODO: normalize the depth (z component) somehow?
	switch(gl_VertexIndex) {
		// first two points: the default line segment
		case 0: {
			gl_Position = vec4(angleA, 0.0, dot(diffA, diffA), 1.0);
			break;
		} case 1: {
			gl_Position = vec4(angleB + 2 * wrap, 0.0, dot(diffB, diffB), 1.0);
			break;
		// those points are only needed if wrap is true
		// otherwise both will have the same position (0) and therefore
		// be degenerate
		} case 2: {
			gl_Position = vec4(wrap * (angleA - 2), 0.0, dot(diffA, diffA), 1.0);
			break;
		} case 3: {
			gl_Position = vec4(wrap * angleB, 0.0, dot(diffB, diffB), 1.0);
			break;
		}
	}
}
