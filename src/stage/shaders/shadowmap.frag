#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 inTexCoord0;
layout(location = 1) in vec2 inTexCoord1;

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(push_constant) uniform MaterialPcrBuf {
	MaterialPcr material;
};

void main() {
	vec2 uv = (material.albedoCoords == 0) ? inTexCoord0 : inTexCoord1;
	vec4 albedo = material.albedo * texture(albedoTex, uv);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	// don't render backfaces. We render with depth clamping (manual
	// workaround if needed) so no problem if the front face is behind
	// the camera
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}
}
