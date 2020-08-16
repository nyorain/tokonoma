#include "shared.glsl"

layout(location = 0) out vec2 outPos;
layout(location = 1) out vec4 outColor;

layout(set = 0, binding = 0) readonly buffer Vertices {
	LightVertex vertices[];
};

layout(set = 0, binding = 1, row_major) readonly uniform Scene {
	UboData scene;
};

layout(push_constant) uniform PCR {
	vec2 pixelSize;
} pcr;

const uint vertexTable[6] = {
	0, 0, 1, 0, 1, 1
};

const float offsetTable[6] = {
	1, -1, -1, 1, -1, 1
};

void main() {
	uint triID = 2 * (gl_VertexIndex / 6);
	uint vertID = gl_VertexIndex % 6;

	vec2 dir = vertices[triID + 1].position - vertices[triID + 0].position;

	LightVertex vert = vertices[triID + vertexTable[vertID]];
	outColor = vert.color;

	vec2 offset = offsetTable[vertID] * vert.normal;
	vec2 pos = vert.position;

	float cosTheta = dot(normalize(dir), offset);
	float sinTheta = sqrt(1 - cosTheta * cosTheta);
	float strength = 1.f / sinTheta;
	pos += strength * 0.005 * offset;

	// dir = (scene.transform * vec4(dir, 0, 0)).xy;
	// offset = (scene.transform * vec4(offset, 0, 0)).xy;
	pos = (scene.transform * vec4(pos, 0, 1)).xy;

	// offset = normalize(offset);
	// float cosTheta = dot(normalize(dir), offset);
	// float sinTheta = sqrt(1 - cosTheta * cosTheta);
	// float strength = 1.f / sinTheta;
	// pos += strength * pcr.pixelSize * offset;

	gl_Position = vec4(pos, 0, 1);
	gl_Position.xy += scene.jitter * gl_Position.w;
	outPos = gl_Position.xy;
}

