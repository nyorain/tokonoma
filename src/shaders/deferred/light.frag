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

// gbuffer
layout(set = 1, binding = 0, input_attachment_index = 0)
	uniform subpassInput inNormal;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;

// NOTE: emission not supported yet
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inEmission;

// layout(set = 1, binding = 3) uniform sampler2DShadow depthTex;
layout(set = 1, binding = 3) uniform sampler2D depthTex;

layout(set = 2, binding = 0, row_major) uniform PointLightBuf {
	PointLight pointLight;
};
layout(set = 2, binding = 0, row_major) uniform DirLightBuf {
	DirLight dirLight;
};

layout(set = 2, binding = 1) uniform sampler2DShadow shadowMap;
layout(set = 2, binding = 1) uniform samplerCubeShadow shadowCube;

const uint SSAO_SAMPLE_COUNT = 32u; // NOTE: sync with deferred/main
layout(set = 3, binding = 0) uniform SSAOSamplerBuf {
	vec4 samples[SSAO_SAMPLE_COUNT];
} ssao;
layout(set = 3, binding = 1) uniform sampler2D ssaoNoiseTex;

layout(push_constant) uniform Show {
	uint mode;
} show;

// TODO: should be in extra pass, blurred afterwards...
float computeSSAO(vec3 pos, vec3 normal) {
	// TODO: easier when using repeated texture sampler
	vec2 noiseSize = textureSize(ssaoNoiseTex, 0);
	vec2 nuv = mod(uv * textureSize(depthTex, 0), noiseSize) / noiseSize;
	vec3 randomVec = texture(ssaoNoiseTex, nuv).xyz;  

	vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
	vec3 bitangent = cross(normal, tangent);
	mat3 TBN = mat3(tangent, bitangent, normal); 

	float occlusion = 0.0;
	float radius = 0.5f;
	for(int i = 0; i < SSAO_SAMPLE_COUNT; ++i) {
		vec3 mapped = TBN * ssao.samples[i].xyz; // From tangent to view-space
		mapped = pos + mapped * radius; 
		mapped = sceneMap(scene.proj, mapped);
		float sampleDepth = texture(depthTex, mapped.xy).r;
		
		// const float bias = 0.025;
		const float bias = 0.0;
		occlusion += (sampleDepth + bias >= mapped.z ? 1.0 : 0.0);  

		// TODO: range check
	}  

	occlusion = occlusion / SSAO_SAMPLE_COUNT;
	return occlusion;
}

void main() {
	vec2 suv = 2 * uv - 1;
	suv.y *= -1.f; // flip y
	float depth = texture(depthTex, uv).r;
	// we can skip light (but not scattering) if depth == 1
	// TODO: currently scattered over main...
	// if(depth == 1) {
	// 	fragColor = vec4(0.0);
	// 	return; // ignore, cleared
	// }

	vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
	vec3 pos = pos4.xyz / pos4.w;

	vec4 sNormal = subpassLoad(inNormal);
	vec4 sAlbedo = subpassLoad(inAlbedo);
	vec4 sEmission = subpassLoad(inEmission);

	// vec3 normal = decodeNormal(sNormal.xy);
	vec3 normal = sNormal.xyz;
	vec3 albedo = sAlbedo.xyz;

	float occlusion = sNormal.w;
	float roughness = sAlbedo.w;
	float metallic = sEmission.w;

	// ssao
	float ao = 0.0;
	if(depth != 1.f) {
		ao = computeSSAO(pos, normal);
	}

	// debug modes
	switch(show.mode) {
	case 1:
		fragColor = vec4(albedo, 0.0);
		return;
	case 2:
		fragColor = vec4(normal, 0.0);
		return;
	case 3:
		fragColor = vec4(pos, 0.0);
		return;
	case 4:
		fragColor = vec4(vec3(pow(depth, 15)), 0.0);
		return;
	case 5:
		fragColor = vec4(vec3(occlusion), 0.0);
		return;
	case 6:
		fragColor = vec4(vec3(metallic), 0.0);
		return;
	case 7:
		fragColor = vec4(vec3(roughness), 0.0);
		return;
	case 8:
		fragColor = vec4(vec3(ao), 0.0);
		return;
	default:
		break;
	}

	// TODO: where does this factor come from? make variable
	float ambientFac = 0.5 * occlusion;

	float shadow = 0;
	vec3 ldir;
	vec3 lightToView;
	vec3 mappedLightPos; // xy is screenspace, z is depth

	bool pcf = (dirLight.flags & lightPcf) != 0;
	if((dirLight.flags & lightDir) != 0) {
		// position on light tex
		vec4 lsPos = dirLight.proj * vec4(pos, 1.0);
		lsPos.xyz /= lsPos.w;
		lsPos.y *= -1; // invert y
		lsPos.xy = 0.5 + 0.5 * lsPos.xy; // normalize for texture access

		if(depth != 1.f) {
			shadow = dirShadow(shadowMap, lsPos.xyz, int(pcf));
		}

		ldir = normalize(dirLight.dir); // TODO: norm could be done on cpu
		lightToView = ldir;

		// TODO: could be done on cpu!
		// mapped position of a directional light
		// manually depth clamp
		mappedLightPos = sceneMap(scene.proj, scene.viewPos - ldir);
		if(mappedLightPos.z != clamp(mappedLightPos.z, 0, 1)) {
			mappedLightPos.xy = vec2(0.0);
		}

		mappedLightPos.z = 1.f; // on far plane
	} else {
		if(depth != 1.f){ 
			if(pcf) {
				float viewDistance = length(scene.viewPos - pos);
				float radius = (1.0 + (viewDistance / 30.0)) / 25.0;  
				shadow = pointShadowSmooth(shadowCube, pointLight.pos,
					pointLight.farPlane, pos, radius);
			} else {
				shadow = pointShadow(shadowCube, pointLight.pos,
					pointLight.farPlane, pos);
			}
		}

		ldir = normalize(pos - pointLight.pos);
		lightToView = normalize(scene.viewPos - pointLight.pos);
		mappedLightPos = sceneMap(scene.proj, pointLight.pos);
	}

	vec3 v = normalize(scene.viewPos - pos);
	vec3 light = dot(normal, -ldir) * albedo; // diffuse only
	// vec3 light = cookTorrance(normal, -ldir, v, roughness, metallic,
		// albedo);

	// TODO: attenuation
	vec3 color = max(shadow * light * dirLight.color, 0.0);
	ambientFac *= ao;
	// TODO: somehow include light color in ssao?
	// color += ambientFac * albedo * normalize(dirLight.color);
	color += ambientFac * albedo;
	fragColor = vec4(color, 0.0);

	vec2 mappedFragPos = sceneMap(scene.proj, pos).xy;
	float scatter = lightScatterDepth(mappedFragPos, mappedLightPos.xy,
		mappedLightPos.z, lightToView, -v, depthTex);
	fragColor.a = scatter;
	// fragColor.rgb += scatter * dirLight.color.rgb;
}
