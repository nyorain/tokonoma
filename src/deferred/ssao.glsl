#include "scene.glsl"

layout(constant_id = 0) const uint ssaoSampleCount = 16;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
	float near;
	float far;
} scene;

layout(set = 1, binding = 0) uniform SSAOSamplerBuf {
	vec4 samples[ssaoSampleCount];
} ssao;

layout(set = 1, binding = 1) uniform sampler2D noiseTex;
layout(set = 1, binding = 2) uniform sampler2D depthTex;
layout(set = 1, binding = 3) uniform sampler2D normalTex;

float computeSSAO(vec2 uv) {
	float z = texture(depthTex, uv).r;
	if(z >= scene.far) { // nothing rendered here
		return 0.f;
	}

	float depth = ztodepth(z, scene.near, scene.far);
	vec3 pos = reconstructWorldPos(uv, scene.invProj, depth);
	vec3 normal = decodeNormal(textureLod(normalTex, uv, 0).xy);

	// TODO: easier when using repeated texture sampler...
	vec2 noiseSize = textureSize(noiseTex, 0);
	vec2 nuv = mod(uv * textureSize(depthTex, 0), noiseSize) / noiseSize;
	vec3 randomVec = texture(noiseTex, nuv).xyz;  

	vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
	vec3 bitangent = cross(normal, tangent);
	mat3 TBN = mat3(tangent, bitangent, normal); 

	float occlusion = 0.0;
	float radius = 0.15;
	const float bias = 0.025;

	// NOTE: lod important for performance, random sampling kills cache
	// the nearer we are, the higher mipmap level we can use since the radius
	// is in world space. If we are near to a surface then even a small
	// world space radius means a huge screen space distance.
	// TODO: currently not used (depth doesn't generate mipmaps...)
	float lod = clamp(radius / z, 0.0, 4.0);
	for(int i = 0; i < ssaoSampleCount; ++i) {
		vec3 samplePos = TBN * ssao.samples[i].xyz; // From tangent to view-space
		samplePos = pos + samplePos * radius; 
		vec3 screenSpace = sceneMap(scene.proj, samplePos);
		screenSpace.xy = clamp(screenSpace.xy, 0.0, 1.0);
		float sampleDepth = textureLod(depthTex, screenSpace.xy, lod).r;
		
		float offz = depthtoz(screenSpace.z, scene.near, scene.far);
		float samplez = sampleDepth;
		float rangeCheck = smoothstep(0.0, 1.0, radius / abs(samplez - z));

		occlusion += (samplez <= offz - bias ? 1.0 : 0.0) * rangeCheck;
	}  

	return 1 - (occlusion / ssaoSampleCount);
}
