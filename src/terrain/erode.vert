#version 450
#extension GL_GOOGLE_include_directive : require

#include "constants.glsl"

layout(location = 0) in vec2 inPos;
layout(location = 1) in float inStrength;
layout(location = 0) out float outStrength;

void main() {
	outStrength = inStrength;	
	gl_Position = vec4(inPos, 0.0, 1.0);
	gl_PointSize = erosionRadius;
}
