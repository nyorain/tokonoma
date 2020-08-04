#version 450

#extension GL_GOOGLE_include_directive : require
#include "subd.glsl"
#include "displace.glsl"

layout(location = 0) in uvec2 inKey;
layout(location = 0) out vec3 outPos;
layout(location = 1) noperspective out vec3 outBary;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
	vec3 pos;
} scene;

layout(set = 0, binding = 1) buffer Vertices { vec4 vertices[]; };
layout(set = 0, binding = 2) buffer Indices { uint indices[]; };

void main() {
	uint idx = inKey.y;
	vec3 v_in[3] = vec3[3](
		vertices[indices[3 * idx + 0]].xyz,
		vertices[indices[3 * idx + 1]].xyz,
		vertices[indices[3 * idx + 2]].xyz
	);

	uint vid = gl_VertexIndex % 3;
	vec3 v_out[3];
	subd(inKey.x, v_in, v_out);
	vec2 bary = subd_bvecs[vid].xy;
	vec3 pos = berp(v_out, bary);
	pos = displace(pos);

	outPos = pos;
	outBary = vec3(
		vid == 0 ? 1.f : 0.f, 
		vid == 1 ? 1.f : 0.f, 
		vid == 2 ? 1.f : 0.f);
	gl_Position = scene.vp * vec4(pos, 1.0);
}
