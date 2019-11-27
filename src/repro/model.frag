#version 450

#extension GL_GOOGLE_include_directive : enable
#include "pbr.glsl"
#include "scene.glsl"
#include "scene.frag.glsl"

const float snapSpeed = 5; // in world units per second
const float snapLength = 20; // valid how long, in seconds

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;
layout(location = 4) in flat uint inMatID;
layout(location = 5) in flat uint inModelID;

layout(location = 0) out float outCol;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	vec3 camPos;
	float time;

	mat4 snapVP;
	float snapTime;

	float near, far;
	float exposure;
} scene;

// material
layout(set = 1, binding = 2, std430) buffer Materials {
	Material materials[];
};

layout(set = 1, binding = 3) uniform texture2D textures[imageCount];
layout(set = 1, binding = 4) uniform sampler samplers[samplerCount];

layout(set = 2, binding = 0) uniform sampler2D snapshotTex;

vec4 readTex(MaterialTex tex) {
	vec2 tuv = (tex.coords == 0u) ? inTexCoord0 : inTexCoord1;
	return texture(sampler2D(textures[tex.id], samplers[tex.samplerID]), tuv);	
}

float mrandom(vec4 seed4) {
	float dot_product = dot(seed4, vec4(12.9898,78.233,45.164,94.673));
	return fract(sin(dot_product) * 43758.5453);
}

void main() {
	Material material = materials[inMatID];

	vec4 albedo = material.albedoFac * readTex(material.albedo);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	vec3 normal = normalize(inNormal);
	if((material.flags & normalMap) != 0u) {
		MaterialTex nt = material.normals;
		vec2 tuv = (nt.coords == 0u) ? inTexCoord0 : inTexCoord1;
		vec4 n = texture(sampler2D(textures[nt.id], samplers[nt.samplerID]), tuv);
		normal = tbnNormal(normal, inPos, tuv, 2.0 * n.xyz - 1.0);
	}

	vec3 snapuv = sceneMap(scene.snapVP, inPos);
	if(snapuv.xy != clamp(snapuv.xy, 0, 1)) {
		discard;
	}

	const vec2 poissonDisk[16] = vec2[]( 
	   vec2( -0.94201624, -0.39906216 ), 
	   vec2( 0.94558609, -0.76890725 ), 
	   vec2( -0.094184101, -0.92938870 ), 
	   vec2( 0.34495938, 0.29387760 ), 
	   vec2( -0.91588581, 0.45771432 ), 
	   vec2( -0.81544232, -0.87912464 ), 
	   vec2( -0.38277543, 0.27676845 ), 
	   vec2( 0.97484398, 0.75648379 ), 
	   vec2( 0.44323325, -0.97511554 ), 
	   vec2( 0.53742981, -0.47373420 ), 
	   vec2( -0.26496911, -0.41893023 ), 
	   vec2( 0.79197514, 0.19090188 ), 
	   vec2( -0.24188840, 0.99706507 ), 
	   vec2( -0.81409955, 0.91437590 ), 
	   vec2( 0.19984126, 0.78641367 ), 
	   vec2( 0.14383161, -0.14100790 ));

	float fac = 0.0;
	int count = 8;
	for(int i = 0; i < count; i++) {
		// we could make the length dependent on the
		// distance behind the first sample or something... (i.e.
		// make the shadow smoother when further away from
		// shadow caster).
		float len = 4 * mrandom(vec4(gl_FragCoord.xyy + 100 * inPos.xyz, i));
		float rid = mrandom(vec4(0.1 * gl_FragCoord.yxy - 32 * inPos.yzx, i));
		int id = int(16.0 * rid) % 16;
		vec2 off = len * poissonDisk[id] / textureSize(snapshotTex, 0).xy;
		// float z = texture(snapshotTex, snap.xy + off).w;
		// float z = depthtoz(texture(snapshotTex, snap.xy + off).r, ubo.near, ubo.far);
		float z = texture(snapshotTex, snapuv.xy + off).r;
		float f = snapuv.z < z ? 1.f : 0.f;
		fac += f / count;
	}

	if(fac == 0.0) {
		discard;
	}

	float dt = scene.time - scene.snapTime;
	float result = distance(scene.camPos, inPos);

	if(dt * snapSpeed < result) {
		discard;
	}

	float low = result / snapSpeed;
	float high = result / snapSpeed + snapLength;
	fac *= 1 - smoothstep(low, high, dt);
	// float dv = clamp(dot(normalize(scene.camPos - inPos), normalize(inNormal)), 0, 1);
	// float dv = smoothstep(-1, 1, dot(normalize(scene.camPos - inPos), normalize(inNormal)));
	float dv = smoothstep(-1, 1, dot(normalize(scene.camPos - inPos), normal));

	float zcurrent = distance(inPos, scene.camPos);
	float zsnap = depthtoz(snapuv.z, scene.near, scene.far);
	// outCol = fac * exp(-scene.exposure * (0.1 + zcurrent) * zsnap);
	// outCol = fac * exp(-scene.exposure * pow(zcurrent, 2) * zsnap);
	// outCol = fac * (1.5 - dv) * exp(-scene.exposure * zcurrent * zsnap);
	outCol = fac * (0.1 + dv) * exp(-scene.exposure * zcurrent * zsnap);
}
