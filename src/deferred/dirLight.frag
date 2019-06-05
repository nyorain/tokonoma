#version 450
#extension GL_GOOGLE_include_directive : enable

// implements the main lightning, we just supply the light parameters
#include "light.glsl"

layout(set = 2, binding = 0, row_major) uniform LightBuf {
	DirLight light;
};
layout(set = 2, binding = 1) uniform sampler2DArrayShadow shadowMap;

void getLightParams(vec3 viewPos, vec3 fragPos, out vec3 ldir,
		out vec3 lcolor, float linearz) {
	lcolor = light.color;
	bool pcf = (light.flags & lightPcf) != 0;
	ldir = normalize(light.dir); // TODO: norm could be done on cpu

	float between;
	uint index = getCascadeIndex(light, linearz, between);

	// color the different levels for debugging
	// switch(index) {
	// 	case 0: lcolor = mix(lcolor, vec3(1, 1, 0), 0.8); break;
	// 	case 1: lcolor = mix(lcolor, vec3(0, 1, 1), 0.8); break;
	// 	case 2: lcolor = mix(lcolor, vec3(1, 0, 1), 0.8); break;
	// 	case 3: lcolor = mix(lcolor, vec3(1, 0, 1), 0.8); break;
	// 	default: lcolor = vec3(1, 1, 1); break;
	// };

	// lcolor *= dirShadowIndex(light, shadowMap, fragPos, index, int(pcf));

	// bad idea, doesn't look good
	float s0 = dirShadowIndex(light, shadowMap, fragPos, index, int(pcf));
	float s1 = dirShadowIndex(light, shadowMap, fragPos, index + 1, int(pcf));
	lcolor *= mix(s0, s1, between);
}
