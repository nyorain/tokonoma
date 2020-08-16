#include "glsl.hpp"

struct Segment {
	vec2 start;
	vec2 end;
	uint material;
	uint _pad0 GLSL_EMPTY;
};

struct Material {
	vec3 albedo;
	float roughness;
	float metallic;
	float _pad0[3] GLSL_EMPTY;
};

struct Light {
	vec3 color;
	float radius;
	vec2 pos;
	float _pad0[2] GLSL_EMPTY;
};

struct LightVertex {
	vec4 color;
	vec2 position;
	vec2 normal;
};

struct UboData {
	mat4 transform;
	vec2 jitter;
	vec2 offset;
	vec2 size;
	float time;
	uint frameID;
	float rayTime;
};
