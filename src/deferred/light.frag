#version 450

#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "pbr.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

// scene
layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
} scene;

// gbuffers
layout(set = 1, binding = 0, input_attachment_index = 0)
	uniform subpassInput inNormal;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inEmission;
// layout(set = 1, binding = 3, input_attachment_index = 3)
// 	uniform subpassInput inDepth;
layout(set = 1, binding = 3)
	uniform sampler2D depthTex;

// light ds aliasing for different light types
// we could also use different pipelines
layout(set = 2, binding = 0, row_major) uniform PointLightBuf {
	PointLight pointLight;
};
layout(set = 2, binding = 0, row_major) uniform DirLightBuf {
	DirLight dirLight;
};

layout(set = 2, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 2, binding = 1) uniform samplerCubeShadow shadowCube;

layout(push_constant) uniform Show {
	uint mode;
} show;

void main() {
	// float depth = subpassLoad(inDepth).r;
	float depth = texture(depthTex, uv).r;
	if(depth == 1) { // nothing rendered here
		fragColor = vec4(0.0);
		// return;
	}

	// reconstruct position from frag coord (uv) and depth
	vec2 suv = 2 * uv - 1; // projected suv
	suv.y *= -1.f; // flip y
	vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
	vec3 pos = pos4.xyz / pos4.w;

	vec4 sNormal = subpassLoad(inNormal);
	vec4 sAlbedo = subpassLoad(inAlbedo);
	vec4 sEmission = subpassLoad(inEmission);

	vec3 normal = decodeNormal(sNormal.xy);
	vec3 albedo = sAlbedo.xyz;
	float roughness = sNormal.w;
	float metallic = sEmission.w;

	// debug output modes
	switch(show.mode) {
	case 1:
		fragColor = vec4(albedo, 0.0);
		return;
	case 2:
		fragColor = vec4(0.5 * normal + 0.5, 0.0);
		return;
	case 3:
		fragColor = vec4(pos, 0.0);
		return;
	case 4:
		fragColor = vec4(vec3(pow(depth, 15)), 0.0);
		return;
	case 6:
		fragColor = vec4(vec3(metallic), 0.0);
		return;
	case 7:
		fragColor = vec4(vec3(roughness), 0.0);
		return;
	default:
		break;
	}

	float attenuation = 1.f;
	vec3 ldir;

	bool pcf = (dirLight.flags & lightPcf) != 0;
	if((dirLight.flags & lightDir) != 0) {
		// position on light tex
		vec4 lsPos = dirLight.proj * vec4(pos, 1.0);
		lsPos.xyz /= lsPos.w;
		lsPos.y *= -1; // invert y
		lsPos.xy = 0.5 + 0.5 * lsPos.xy; // normalize for texture access
		attenuation *= dirShadow(shadowMap, lsPos.xyz, int(pcf));

		ldir = normalize(dirLight.dir); // TODO: norm could be done on cpu
	} else {
		if(pcf) {
			// TODO: make radius parameters configurable,
			// depend on scene size
			float viewDistance = length(scene.viewPos - pos);
			float radius = (1.0 + (viewDistance / 30.0)) / 25.0;  
			attenuation *= pointShadowSmooth(shadowCube, pointLight.pos,
				pointLight.farPlane, pos, radius);
		} else {
			attenuation *= pointShadow(shadowCube, pointLight.pos,
				pointLight.farPlane, pos);
		}

		ldir = pos - pointLight.pos;
		float lightDistance = length(pos - pointLight.pos);
		ldir /= lightDistance;

		float denom = 
			pointLight.attenuation.x + // constant
			pointLight.attenuation.y * lightDistance + // linear
			pointLight.attenuation.z * (lightDistance * lightDistance);
		attenuation *= 1 / denom;
	}

	// vec3 light = dot(normal, -ldir) * albedo; // diffuse only
	vec3 v = normalize(scene.viewPos - pos);
	vec3 light = cookTorrance(normal, -ldir, v, roughness, metallic,
		albedo);

	vec3 color = max(attenuation * light * pointLight.color, 0.0);
	fragColor = vec4(color, 0.0);


	// TODO! temporary light scattering experiments
	vec2 mappedFragPos = sceneMap(scene.proj, pos).xy;
	vec3 mappedLightPos = sceneMap(scene.proj, pointLight.pos);
	vec3 lightToView = normalize(scene.viewPos - pointLight.pos);
	float scatter = lightScatterDepth(mappedFragPos, mappedLightPos.xy,
		mappedLightPos.z, lightToView, -v, depthTex);
	fragColor.a = scatter;
}
