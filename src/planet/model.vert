#version 460

#extension GL_GOOGLE_include_directive : enable
#include "subd.glsl"

layout(location = 0) in uvec2 inKey;
layout(location = 0) out vec3 outPos;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp;
	vec3 pos;
} scene;

// NOTE: we render a simple cube as planet. We could replace this
// with manual calculation inside the shader.
layout(set = 0, binding = 1) buffer Vertices { vec4 vertices[]; };
layout(set = 0, binding = 2) buffer Indices { uint indices[]; };
layout(set = 0, binding = 3) uniform samplerCube heightmap;

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
	pos = (1 + 0.1 * texture(heightmap, pos).r) * normalize(pos);
	pos = 6360 * pos;

	outPos = pos;
	gl_Position = scene.vp * vec4(pos, 1.0);
}
