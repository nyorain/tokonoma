#version 450
#extension GL_GOOGLE_include_directive : require

#include "constants.glsl"
#include "math.glsl"

layout(location = 0) in float inStrength;
layout(location = 0) out float outStrength;

void main() {
	/*
	vec2 toCenter = erosionRadius * (0.5 - gl_PointCoord);
	float distSqr = dot(toCenter, toCenter);

	float deriv = 2.0;
	float amp = 0.5 / (pi * deriv * deriv);
	amp *= 4.0;
	float strength = amp * exp(-0.5 * distSqr / (deriv * deriv));
	*/

	vec2 toCenter = 2 * (0.5 - gl_PointCoord);
	float strength = max(1 - length(toCenter), 0.0) / (0.5 * erosionRadius * erosionRadius);
	outStrength = strength * inStrength;
}
