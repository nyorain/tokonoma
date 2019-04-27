#version 450
#extension GL_GOOGLE_include_directive : enable

// implements the main lightning, we just supply the light parameters
#include "light.glsl"

layout(set = 2, binding = 0, row_major) uniform LightBuf {
	PointLight light;
};
layout(set = 2, binding = 1) uniform samplerCubeShadow shadowCube;

void getLightParams(vec3 viewPos, vec3 fragPos, out vec3 ldir,
		out vec3 lcolor) {
	lcolor = light.color;
	bool pcf = (light.flags & lightPcf) != 0;
	if(pcf) {
		// TODO: make radius parameters configurable,
		// depend on scene size
		float viewDistance = length(viewPos - fragPos);
		float radius = (1.0 + (viewDistance / 30.0)) / 25.0;  
		lcolor *= pointShadowSmooth(shadowCube, light.pos, light.farPlane,
			fragPos, radius);
	} else {
		lcolor *= pointShadow(shadowCube, light.pos, light.farPlane, fragPos);
	}

	ldir = fragPos - light.pos;
	float lightDistance = length(fragPos - light.pos);
	ldir /= lightDistance;

	float denom = 
		light.attenuation.x + // constant
		light.attenuation.y * lightDistance + // linear
		light.attenuation.z * (lightDistance * lightDistance);
	lcolor *= 1 / denom;
}
