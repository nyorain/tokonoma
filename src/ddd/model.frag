#version 450

#extension GL_GOOGLE_include_directive : enable
#include "pbr.glsl"
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inLightPos; // position from pov light; for shadow
layout(location = 3) in vec2 inTexCoord0;
layout(location = 4) in vec2 inTexCoord1;

layout(location = 0) out vec4 outCol;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	vec3 viewPos; // camera position. For specular light
} scene;

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(set = 1, binding = 1) uniform sampler2D metalRoughTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;

// TODO: not allowed by glsl, remove aliasing
// alias for different light types
layout(set = 3, binding = 0, row_major) uniform DirLightBuf {
	DirLight dirLight;
};
layout(set = 3, binding = 0, row_major) uniform PointLightBuf {
	PointLight pointLight;
};

layout(set = 3, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 3, binding = 1) uniform samplerCubeShadow shadowCube;

// factors
layout(push_constant) uniform MaterialPcrBuf {
	MaterialPcr material;
};

// NOTE: tangent and bitangent could also be passed in for each vertex
// then we could already compute light and view positiong in tangent space
vec3 getNormal() {
	vec3 n = normalize(inNormal);
	if((material.flags & normalMap) == 0u) {
		return n;
	}

	vec2 uv = (material.normalCoords == 0u) ? inTexCoord0 : inTexCoord1;
	return tbnNormal(n, inPos, uv, normalTex);
}

void main() {
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	vec2 auv = (material.albedoCoords == 0u) ? inTexCoord0 : inTexCoord1;
	vec4 albedo = material.albedo * texture(albedoTex, auv);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	vec3 normal = getNormal();
	if(!gl_FrontFacing) { // flip normal, see gltf spec
		normal *= -1;
	}

	vec2 mruv = (material.metalRoughCoords == 0u) ? inTexCoord0 : inTexCoord1;
	vec4 mr = texture(metalRoughTex, mruv);
	float metalness = material.metallic * mr.b;
	float roughness = material.roughness * mr.g;

	vec2 ouv = (material.occlusionCoords == 0u) ? inTexCoord0 : inTexCoord1;
	float ambientFac = 0.1 * texture(occlusionTex, ouv).r;
	// float diffuseFac = 0.5f;
	// float specularFac = 0.5f;
	// float shininess = 64.f;

	float shadow;
	vec3 ldir;

	// TODO: some features missing, compare deferred renderer
	// point light: attenuation no implemented here atm
	bool pcf = (dirLight.flags & lightPcf) != 0;
	if((dirLight.flags & lightDir) != 0) {
		shadow = dirShadow(shadowMap, inLightPos, int(pcf));
		ldir = normalize(dirLight.dir);
	} else {
		ldir = normalize(inPos - pointLight.pos);
		if(pcf) {
			float radius = 0.005;
			shadow = pointShadowSmooth(shadowCube, pointLight.pos,
				pointLight.radius, inPos, radius);
		} else {
			shadow = pointShadow(shadowCube, pointLight.pos,
				pointLight.radius, inPos);
		}
	}

	vec3 v = normalize(scene.viewPos - inPos);
	vec3 light = cookTorrance(normal, -ldir, v, roughness, metalness,
		albedo.xyz);
	light *= shadow;
	outCol.rgb = dirLight.color * light;
	outCol.rgb += ambientFac * albedo.rgb;

	outCol.a = albedo.a;
	if(material.alphaCutoff == -1.f) { // alphaMode opque
		outCol.a = 1.f;
	}
}
