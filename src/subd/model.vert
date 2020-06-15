#version 460
#define FBM_OCTAVES 6

#extension GL_GOOGLE_include_directive : enable
#include "noise.glsl"
#include "subd.glsl"

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
	vec3 pos;
} scene;

// real scene
layout(set = 0, binding = 1) buffer Vertices { vec4 vertices[]; };
layout(set = 0, binding = 2) buffer Indices { uint indices[]; };

// enough for our small dummy scene
// const vec3 vertices[] = {
// 	{-1, 0, -1}, // 4 outlining points ...
// 	{1, 0, -1},
// 	{1, 0, 1},
// 	{-1, 0, 1},
// };
// 
// const uint indices[] = {
// 	0, 1, 2,
// 	0, 2, 3
// };

layout(location = 0) in uvec2 key;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;
layout(location = 2) out float outHeight;
layout(location = 3) out vec3 outPos;

vec3 colorMap[] = {
	vec3(0.1, 0.1, 0.1),
	vec3(0.2, 0.2, 0.2),
	vec3(0.3, 0.3, 0.3),
	vec3(0.4, 0.4, 0.4),
	vec3(0.5, 0.5, 0.5),

	vec3(1, 0, 0),
	vec3(0, 1, 0),
	vec3(0, 0, 1),
	vec3(1, 0, 1),
	vec3(1, 1, 0),
	vec3(0, 1, 1),
	vec3(0, 0.5, 1),
	vec3(0.5, 0.5, 1),
	vec3(1, 0.5, 1),
	vec3(1, 0.5, 0.5),
	vec3(1, 1, 0.5),
	vec3(1, 0.5, 1),
	vec3(0.5, 0.5, 1),
	vec3(0.5, 1, 1),

	vec3(1, 0, 0),
	vec3(0, 1, 0),
	vec3(0, 0, 1),
	vec3(1, 0, 1),
	vec3(1, 1, 0),
	vec3(0, 1, 1),
	vec3(0, 0.5, 1),
	vec3(0.5, 0.5, 1),
	vec3(1, 0.5, 1),
	vec3(1, 0.5, 0.5),

	vec3(1, 1, 1),
};

// vec2 morphVertex(vec2 vertex,float morphK){
// 	float patchTessFactor = 4.0;
// 	vec2 fracPart = fract(vertex * patchTessFactor * 0.5);
// 	fracPart *= 2.0 /patchTessFactor;
// 	vec2 intPart = floor(vertex * patchTessFactor * 0.5);
// 	vec2 signVec = mod(intPart, 2.0) *vec2(-2.0) +vec2(1.0);
// 	return vertex.xy - (signVec * fracPart) * morphK;
// }
vec2 morphVertex(vec2 vertex, float morphK) {
	// vertex.x -= 0.5 * morphK;
	vertex.x -= morphK;
	// vertex.y += 0.5 * morphK;
	return vertex;
}

float desiredLOD(uint key, uint ini) {
	vec3 v_in[3] = vec3[3](
		vertices[indices[3 * ini + 0]].xyz,
		vertices[indices[3 * ini + 1]].xyz,
		vertices[indices[3 * ini + 2]].xyz
	);

	vec3 tri[3];
	subd(key, v_in, tri);
	float d0;
	vec3 d1;
	vec3 hyp = 0.5 * (displace(tri[1], d1, d0) + displace(tri[2], d1, d0));
	return distanceToLOD(distance(scene.pos, hyp));
}

void main() {
	uint idx = key.y;
	vec3 v_in[3] = vec3[3](
		vertices[indices[3 * idx + 0]].xyz,
		vertices[indices[3 * idx + 1]].xyz,
		vertices[indices[3 * idx + 2]].xyz
	);

	uint vid = gl_VertexIndex % 3;
	// mat3 xf = keyToMat(key);
	// vec2 bary = (xf * subd_bvecs[vid]).xy;
	// vec3 pos = berp(v_in, bary);
	// vec3 pos = subd(key.x, v_in, vid);

	vec3 v_out[3];
	subd(key.x, v_in, v_out);
	vec2 bary = subd_bvecs[vid].xy;
	// vec3 pos = berp(v_out, bary);

	float d0;
	vec3 d1;
	// vec3 hyp = 0.5 * (displace(v_out[1], d1, d0) + displace(v_out[2], d1, d0));
	// vec3 pos = displace(pos, outNormal, outHeight);

	// float k = 1 - fract(distanceToLOD(distance(pos, scene.pos)) - 0.5);
	// float lod = distanceToLOD(distance(scene.pos, hyp));

	/*
	float lod = desiredLOD((key.x <= 1) ? key.x : key.x >> 1u, key.y);
	// float lod = desiredLOD(key.x, key.y);
	float k = 0;
	if(vid == 0) {
		k = 1 - fract(lod);
		// if(uint(lod) != findMSB(key.x)) {
		// 	k = 0.0;
		// }

		// bary = morphVertex(bary, k);
		bool d = (v_in[0] - 0.5 * (v_out[2] + v_out[1])).x > 0;
		float f1 = d ? 1.f : 0.f;
		float f2 = !d ? 1.f : 0.f;

		bool c = (v_in[0] - 0.5 * (v_out[2] + v_out[1])).y > 0;
		if(c) {
			f1 *= -1;
			f2 *= -1;
		}

		// if((key.x & 2) == 2) {
		// 	f1 *= -1;
		// 	f2 *= -1;
		// }
		bary = vec2(f1 * k, -f2 * k);
	}
	*/

	vec3 pos = berp(v_out, bary);
	pos = displace(pos, outNormal, outHeight);

	uint cidx = findMSB(key.x);
	outColor = vec3(0.0);
	// outColor = 0.5 * vec3(k, 0, 1-k);
	outColor += 0.5 * colorMap[cidx];

	// https://www.enkisoftware.com/devlogpost-20150131-1-Normal-generation-in-the-pixel-shader
	outPos = pos - scene.pos;
	gl_Position = vec4(pos, 1.0);

#ifndef NO_TRANSFORM
	gl_Position = scene.vp * gl_Position;
#endif
}
