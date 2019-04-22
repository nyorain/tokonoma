#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec3 inPos;

// material
layout(set = 0, binding = 0, row_major) uniform LightBuf {
	PointLight light;
};

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(push_constant) uniform MaterialPcrBuf {
	MaterialPcr material;
};

void main() {
	// NOTE: logic here has to be that weird (two independent if statements)
	// since otherwise i seem to trigger a bug (probably in driver or llvm?)
	// on mesas vulkan-radeon 19.0.2
	vec4 albedo = material.albedo * texture(albedoTex, inUV);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	// store depth: distance from light to pixel in that direction
	float farPlane = light.pos.w;
    float dist = length(inPos - light.pos.xyz);

	// depth bias, roughly like vkCmdSetDepthBias
	// TODO: should be configurable, e.g. use push constant
	const float bias = 0.01;
	dist += bias;
	// XXX: use slope as with vkCmdSetDepthBias; probably not perfect
	// read up in vulkan spec how it's implemented
	const float biasSlope = 5.0;
	dist += biasSlope * max(abs(dFdx(inPos.z)), abs(dFdy(inPos.z)));

    gl_FragDepth = dist / farPlane; // map to [0, 1]; required for depth output
}

