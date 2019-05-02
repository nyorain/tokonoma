#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out float blurred;

layout(set = 0, binding = 0) uniform sampler2D ssaoTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;

layout(push_constant) uniform Params {
	uint horizontal;
};

// 9x9 gauss kernel factors
const float kernel[] = {0.04168, 0.03678, 0.02528, 0.01353, 0.00564};

// don't blur over depth edges
// when edges are more than 1 / maxInvDist away, no ssao blur will
// happen
const float maxInvDist = 20.f;

vec2 pixelSize = 1.f / textureSize(ssaoTex, 0);
float centerDepth = texture(depthTex, uv).r;

void accumblur(vec2 off, uint i, inout float accum, inout float total) {
	vec2 uvi = uv + i * off;
	if(uvi != clamp(uvi, 0.0, 1.0)) { // border condition: ignore
		return;
	}

	float sampleDepth = texture(depthTex, uvi).r;
	float sampleAo = texture(ssaoTex, uvi).r;

	float dist = abs(sampleDepth - centerDepth);
	float fac = kernel[i] * smoothstep(0.0, 1.0, 1 - maxInvDist * dist);
	total += fac;
	accum += fac * sampleAo;
}

void main() {
	vec2 off = pixelSize * vec2(horizontal, 1 - horizontal);
	float accum = kernel[0] * texture(ssaoTex, uv).r;
	float total = kernel[0];

	for(uint i = 1u; i < kernel.length(); ++i) {
		accumblur(off, i, accum, total);
	}

	off = -off;
	for(uint i = 1u; i < kernel.length(); ++i) {
		accumblur(off, i, accum, total);
	}

	blurred = accum / total;
}

