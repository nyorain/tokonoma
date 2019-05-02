#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 inUV;

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(push_constant) uniform MaterialPcrBuf {
	MaterialPcr material;
};

void main() {
	vec4 albedo = material.albedo * texture(albedoTex, inUV);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	// NOTE: logic here has to be that weird (two independent if statements)
	// since otherwise i seem to trigger a bug (probably in driver or llvm?)
	// on mesas vulkan-radeon 19.0.2
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}
}