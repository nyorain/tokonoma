#version 460
#define FBM_OCTAVES 6

#extension GL_GOOGLE_include_directive : enable
#include "noise.glsl"
#include "subd.glsl"

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
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

	vec3(1, 1, 1),
};

void main() {
	uint idx = key.y;
	vec3 v_in[3] = vec3[3](
		vertices[indices[3 * idx + 0]].xyz,
		vertices[indices[3 * idx + 1]].xyz,
		vertices[indices[3 * idx + 2]].xyz
	);

	uint vid = gl_VertexIndex % 3;
	vec3 pos = subd(key.x, v_in, vid);
	pos = displace(pos, outNormal, outHeight);

	uint cidx = findMSB(key.x);
	outColor = colorMap[cidx];

	gl_Position = vec4(pos, 1.0);

#ifndef NO_TRANSFORM
	gl_Position = scene.vp * gl_Position;
	gl_Position.y = -gl_Position.y;
#endif
}
