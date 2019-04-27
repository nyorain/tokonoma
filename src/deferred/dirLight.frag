#version 450
#extension GL_GOOGLE_include_directive : enable

// implements the main lightning, we just supply the light parameters
#include "light.glsl"

layout(set = 2, binding = 0, row_major) uniform LightBuf {
	DirLight light;
};
layout(set = 2, binding = 1) uniform sampler2DShadow shadowMap;

void getLightParams(vec3 viewPos, vec3 fragPos, out vec3 ldir,
		out vec3 lcolor) {
	lcolor = light.color;
	bool pcf = (light.flags & lightPcf) != 0;

	vec3 lsPos = sceneMap(light.proj, fragPos);
	lcolor *= dirShadow(shadowMap, lsPos, int(pcf));
	ldir = normalize(light.dir); // TODO: norm could be done on cpu
}
