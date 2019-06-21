#version 460

#extension GL_GOOGLE_include_directive : enable
#include "spharm.glsl"

layout(location = 0) in vec3 uvw;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 1, std430) buffer SSBO {
	vec3 coeffs[9];
};

void main() {
	vec3 nrm = normalize(uvw);
	vec3 col = evalSH(nrm, coeffs);
	outColor = vec4(1.0 - exp(-col), 1.0); // tonemap
}
