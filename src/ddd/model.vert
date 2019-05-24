#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord0;
layout(location = 3) in vec2 inTexCoord1;

layout(location = 0) out vec3 outPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec3 outLightPos;
layout(location = 3) out vec2 outTexCoord0;
layout(location = 4) out vec2 outTexCoord1;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj; // view and pojection
} scene;

layout(set = 2, binding = 0, row_major) uniform Model {
	mat4 matrix; // model matrix
	mat4 normal; // normal matrix (transpose(inverse(matrix))); effectively 3x3
} model;

// light
layout(set = 3, binding = 0, row_major) uniform LightBuf {
	DirLight light;
};

void main() {
	outNormal = mat3(model.normal) * inNormal;
	outTexCoord0 = inTexCoord0;
	outTexCoord1 = inTexCoord1;

	vec4 m = model.matrix * vec4(inPos, 1.0);
	outPos = m.xyz / m.w;

	vec4 l = (light.proj * m);
	l /= l.w;
	l.y *= -1; // invert y
	l.xy = 0.5 + 0.5 * l.xy; // normalize for texture access
	outLightPos = l.xyz;

	gl_Position = scene.proj * m;
	gl_Position.y = -gl_Position.y;
}
