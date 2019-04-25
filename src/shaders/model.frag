#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "scene.frag.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inLightPos; // position from pov light; for shadow
layout(location = 3) in vec2 inUV;

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

	return tbnNormal(n, inPos, inUV, normalTex);
}

void main() {
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	vec4 albedo = material.albedo * texture(albedoTex, inUV);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	vec3 normal = getNormal();
	if(!gl_FrontFacing) { // flip normal, see gltf spec
		normal *= -1;
	}
	// TODO: actually use them...
	// vec2 mr = texture(metalRoughTex, inUV).rg;
	// float metalness = material.matallic * mr.b;
	// float roughness = material.roughness * mr.g;

	// TODO: remove random factors, implement pbr
	float ambientFac = 0.1 * texture(occlusionTex, inUV).r;
	float diffuseFac = 0.5f;
	float specularFac = 0.5f;
	float shininess = 64.f;

	float shadow;
	vec3 ldir;

	bool pcf = (dirLight.flags & lightPcf) != 0;
	if((dirLight.flags & lightDir) != 0) {
		shadow = dirShadow(shadowMap, inLightPos, int(pcf));
		ldir = normalize(dirLight.dir); // TODO: norm could be done on cpu
	} else {
		ldir = normalize(inPos - pointLight.pos);
		if(pcf) {
			float radius = 0.005;
			shadow = pointShadowSmooth(shadowCube, pointLight.pos,
				pointLight.farPlane, inPos, radius);
		} else {
			shadow = pointShadow(shadowCube, pointLight.pos,
				pointLight.farPlane, inPos);
		}
	}

	// diffuse
	vec4 col = vec4(0.0);
	float lfac = diffuseFac * max(dot(normal, -ldir), 0.0); // diffuse

	// specular, blinn phong
	vec3 vdir = normalize(inPos - scene.viewPos);
	vec3 halfway = normalize(-ldir - vdir);
	lfac += specularFac * pow(max(dot(normal, halfway), 0.0), shininess);

	lfac *= shadow;
	lfac += ambientFac; // ambient always added, doesn't care for shadow

	col += vec4(lfac * dirLight.color, 1.0) * albedo;
	outCol = col;
	if(material.alphaCutoff == -1.f) { // alphaMode opque
		outCol.a = 1.f;
	}
}
