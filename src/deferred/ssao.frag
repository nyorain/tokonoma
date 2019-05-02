#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(constant_id = 0) const uint ssaoSampleCount = 64;
layout(location = 0) in vec2 uv;
layout(location = 0) out float outSSAO;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
	float nearPlane;
	float farPlane;
} scene;

layout(set = 1, binding = 0) uniform SSAOSamplerBuf {
	vec4 samples[ssaoSampleCount];
} ssao;

layout(set = 1, binding = 1) uniform sampler2D noiseTex;
layout(set = 1, binding = 2) uniform sampler2D depthTex;
layout(set = 1, binding = 3) uniform sampler2D normalTex;

float computeSSAO(vec3 pos, vec3 normal, float depth) {
	// TODO: easier when using repeated texture sampler...
	vec2 noiseSize = textureSize(noiseTex, 0);
	vec2 nuv = mod(uv * textureSize(depthTex, 0), noiseSize) / noiseSize;
	vec3 randomVec = texture(noiseTex, nuv).xyz;  

	vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
	vec3 bitangent = cross(normal, tangent);
	mat3 TBN = mat3(tangent, bitangent, normal); 

	const float near = scene.nearPlane;
	const float far = scene.farPlane;
	// float z = depthtoz(depth, near, far);
	float z = depth;

	float occlusion = 0.0;
	float radius = 0.15;
	const float bias = 0.025;

	// NOTE: lod important for performance, random sampling kills cache
	// the nearer we are, the higher mipmap level we can use since the radius
	// is in world space. If we are near to a surface then even a small
	// world space radius means a huge screen space distance.
	float lod = clamp(radius / z, 0.0, 6.0);
	for(int i = 0; i < ssaoSampleCount; ++i) {
		vec3 samplePos = TBN * ssao.samples[i].xyz; // From tangent to view-space
		samplePos = pos + samplePos * radius; 
		vec3 screenSpace = sceneMap(scene.proj, samplePos);
		screenSpace.xy = clamp(screenSpace.xy, 0.0, 1.0);
		float sampleDepth = textureLod(depthTex, screenSpace.xy, lod).r;
		
		float offz = depthtoz(screenSpace.z, near, far);
		// float samplez = depthtoz(sampleDepth, near, far);
		float samplez = sampleDepth;
		float rangeCheck = smoothstep(0.0, 1.0, radius / abs(samplez - z));

		occlusion += (samplez <= offz - bias ? 1.0 : 0.0) * rangeCheck;
	}  

	return 1 - (occlusion / ssaoSampleCount);
}

void main() {
	float depth = texture(depthTex, uv).r;
	// if(depth == 1.f) { // nothing rendered here
	if(depth >= 1000.f) { // nothing rendered here
		outSSAO = 0.f;
		return;
	}

	float ndepth = ztodepth(depth, scene.nearPlane, scene.farPlane); // TODO

	// reconstruct position from frag coord (uv) and depth
	vec2 suv = 2 * uv - 1; // projected suv
	suv.y *= -1.f; // flip y
	vec4 pos4 = scene.invProj * vec4(suv, ndepth, 1.0);
	vec3 fragPos = pos4.xyz / pos4.w;

	vec3 normal = decodeNormal(textureLod(normalTex, uv, 0).xy);
	outSSAO = computeSSAO(fragPos, normal, depth);
}
