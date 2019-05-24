#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(location = 0) out vec2 outTexCoord0;
layout(location = 1) out vec2 outTexCoord1;
layout(location = 2) out vec3 outPos; // in global space

layout(set = 0, binding = 0, row_major) uniform Light {
	PointLight light;
};

layout(set = 2, binding = 0, row_major) uniform Model {
	mat4 matrix; // model matrix
} model;

layout(push_constant) uniform Face {
	layout(offset = 64) uint id;
} face;

void main() {
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inTexCoord1;

	vec4 m = model.matrix * vec4(inPos, 1.0); // global space
	outPos = m.xyz / m.w;

	gl_Position = light.proj[face.id] * m;
	// gl_Position.y = -gl_Position.y;
}


