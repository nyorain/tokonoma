#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(location = 0) out vec2 outTexCoord0;
layout(location = 1) out vec2 outTexCoord1;

layout(set = 0, binding = 0, row_major) uniform LightBuf {
	DirLight light;
};

layout(set = 2, binding = 0, row_major) uniform Model {
	mat4 matrix;
} model;

void main() {
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inTexCoord1;

	gl_Position = light.proj * model.matrix * vec4(inPos, 1.0);
	gl_Position.y = -gl_Position.y;
}

