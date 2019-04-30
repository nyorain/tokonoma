#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 outPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out float outLinDepth;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj; // view and pojection
	mat4 _invProj;
	vec3 viewPos;
	float near, far;
} scene;

layout(set = 2, binding = 0, row_major) uniform Model {
	mat4 matrix; // model matrix
	mat4 normal; // normal matrix (transpose(inverse(matrix))); effectively 3x3
} model;

void main() {
	outNormal = mat3(model.normal) * inNormal;
	outUV = inUV;

	vec4 m = model.matrix * vec4(inPos, 1.0);
	outPos = m.xyz / m.w;

	gl_Position = scene.proj * m;
	gl_Position.y = -gl_Position.y;

	// TODO: or pass view and projection seperately...?
	outLinDepth = depthtoz(gl_Position.z / gl_Position.w, scene.near, scene.far);
}
