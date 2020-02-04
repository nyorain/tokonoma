#version 460

#extension GL_GOOGLE_include_directive : enable
#include "subd.glsl"
#include "noise.glsl"

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
} scene;

// for a real scene:
//
// layout(set = 0, binding = 1) buffer Vertices { vec3 vertices[]; };
// layout(set = 0, binding = 2) buffer Indices { uint indices[]; };

// enough for our small dummy scene
const vec3 vertices[] = {
	{-1, 0, -1}, // 4 outlining points ...
	{1, 0, -1},
	{1, 0, 1},
	{-1, 0, 1},
};

const uint indices[] = {
	0, 1, 2,
	0, 2, 3
};

// per-vertex input
layout(location = 0) in uvec2 key;

void main() {
	uint idx = key.y;
	vec3 v_in[3] = vec3[3](
		vertices[indices[3 * idx + 0]],
		vertices[indices[3 * idx + 1]],
		vertices[indices[3 * idx + 2]]
	);

	uint vid = gl_VertexIndex % 3;
	vec3 pos = subd(key.x, v_in, vid);
	pos.y += 0.5 * fbm(2 * pos.xz);

	gl_Position = vec4(pos, 1.0);

#ifndef NO_TRANSFORM
	gl_Position = scene.vp * gl_Position;
	gl_Position.y = -gl_Position.y;
#endif
}
