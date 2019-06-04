#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 inTexCoord0;
layout(location = 1) in vec2 inTexCoord1;
layout(location = 2) flat in uint inMatID;

// material
layout(set = 1, binding = 2) buffer Materials {
	Material materials[];
};

layout(set = 1, binding = 3) uniform texture2D textures[32];
layout(set = 1, binding = 4) uniform sampler samplers[8];

vec4 readTex(MaterialTex tex) {
	vec2 tuv = (tex.coords == 0u) ? inTexCoord0 : inTexCoord1;
	return texture(sampler2D(textures[tex.id], samplers[tex.samplerID]), tuv);	
}

void main() {
	Material material = materials[inMatID];
	vec4 albedo = material.albedoFac * readTex(material.albedo);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	// don't render backfaces. We render with depth clamping (manual
	// workaround if needed) so no problem if the front face is behind
	// the near plane
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}
}
