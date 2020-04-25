#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec3 inPos;

// material
layout(set = 0, binding = 0, row_major) uniform LightBuf {
	PointLight light;
};

#ifdef SCENE
	layout(location = 1) in vec2 inTexCoord0;
	layout(location = 2) in vec2 inTexCoord1;
	layout(location = 3) flat in uint inMatID;

	// material
	layout(set = 1, binding = 2) buffer Materials {
		Material materials[];
	};

	layout(set = 1, binding = 3) uniform texture2D textures[imageCount];
	layout(set = 1, binding = 4) uniform sampler samplers[samplerCount];

	vec4 readTex(MaterialTex tex) {
		vec2 tuv = (tex.coords == 0u) ? inTexCoord0 : inTexCoord1;
		return texture(sampler2D(textures[tex.id], samplers[tex.samplerID]), tuv);	
	}
#endif // SCENE

void main() {
#ifdef SCENE
	Material material = materials[inMatID];
	vec4 albedo = material.albedoFac * readTex(material.albedo);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	// don't render backfaces by default
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}
#endif // SCENE

	// store depth: distance from light to pixel in that direction
	// float farPlane = light.farPlane;
	float farPlane = light.radius;
    float dist = length(inPos - light.pos);

    gl_FragDepth = dist / farPlane; // map to [0, 1]; required for depth output
}

