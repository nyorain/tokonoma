#version 450

#extension GL_GOOGLE_include_directive : enable
#include "pbr.glsl"
#include "scene.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;
layout(location = 4) in flat uint inMatID;
layout(location = 5) in flat uint inModelID;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	vec3 viewPos;
	float near, far;
} scene;

// material
layout(set = 1, binding = 2, std430) buffer Materials {
	Material materials[];
};

layout(set = 1, binding = 3) uniform texture2D textures[imageCount];
layout(set = 1, binding = 4) uniform sampler samplers[samplerCount];

vec4 readTex(MaterialTex tex) {
	vec2 tuv = (tex.coords == 0u) ? inTexCoord0 : inTexCoord1;
	return texture(sampler2D(textures[tex.id], samplers[tex.samplerID]), tuv);	
}

void main() {
	Material material = materials[inMatID];

	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	vec4 albedo = material.albedoFac * readTex(material.albedo);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}
}

