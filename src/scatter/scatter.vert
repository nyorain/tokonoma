#version 460

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) noperspective out vec2 uv;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 vp; // view and pojection
	mat4 _vpPrev;
	mat4 _vpInv;
	vec3 pos;
	float _;
	vec2 jitter;
} scene;

layout(set = 1, binding = 0, row_major) uniform PointLightBuf {
	PointLight pointLight;
};

void main() {
	// Buffer-free cube generation, using triangle strip
	// see e.g. "Eric Lengyel, Game Engine Gems 3" or this discussion:
	// https://twitter.com/Donzanoid/status/616370134278606848
	int mask = (1 << gl_VertexIndex);
	float x = 2 * float((0x287a & mask) != 0) - 1;
	float y = 2 * float((0x02af & mask) != 0) - 1;
	float z = 2 * float((0x31e3 & mask) != 0) - 1;
	vec3 pos = pointLight.pos + pointLight.radius * vec3(x, y, z);
	vec4 proj = scene.vp * vec4(pos, 1.0);

	// TODO: maybe don't jitter here?
	// proj.xy += scene.jitter * proj.w; // jitter in ndc space
	// proj.y = -proj.y;
	gl_Position = proj;

	uv = 0.5 + 0.5 * (proj.xy / proj.w);
}

