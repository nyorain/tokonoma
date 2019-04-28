#version 450

#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "pbr.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out float scatter;

// scene
layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
} scene;

layout(set = 1, binding = 0) uniform sampler2D depthTex;
layout(set = 2, binding = 0, row_major) uniform LightBuf {
	DirLight light;
};

void main() {
	// TODO
	scatter = 0.f;
}
