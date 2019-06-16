layout(set = 0, binding = 0) uniform sampler2D ssaoTex;
layout(set = 0, binding = 1) uniform sampler2D depthTex;

layout(push_constant) uniform Params {
	uint horizontal;
};

// normal 9x9 gauss kernel factors; we don't make use of linear sampling here
// since we have to check each pixel
const float kernel[] = {0.20416, 0.18017, 0.12383, 0.06628, 0.02763};

// don't blur over depth edges
// when edges are more than maxDist away, ssao won't be blurred
// values should depend on scene size
// TODO: should probably be configurable from the outside, via ubo or sth.
const float maxDist = 0.05f;
const float maxInvDist = 1 / maxDist;

void accumblur(vec2 uv, float fac, inout float accum, inout float total,
		float centerDepth) {
	if(uv != clamp(uv, 0.0, 1.0)) { // border condition: ignore
		return;
	}

	float sampleDepth = textureLod(depthTex, uv, 0).r;
	float sampleAo = textureLod(ssaoTex, uv, 0).r;

	float dist = abs(sampleDepth - centerDepth);
	fac *= smoothstep(0.0, 1.0, 1 - maxInvDist * dist);
	total += fac;
	accum += fac * sampleAo;
}

float ssaoBlur(vec2 uv) {
	float centerDepth = texture(depthTex, uv).r;
	vec2 pixelSize = 1.f / textureSize(ssaoTex, 0);

	vec2 off = pixelSize * vec2(horizontal, 1 - horizontal);
	float accum = kernel[0] * textureLod(ssaoTex, uv, 0).r;
	float total = kernel[0];

	for(uint i = 1u; i < kernel.length(); ++i) {
		accumblur(uv + i * off, kernel[i], accum, total, centerDepth);
		accumblur(uv - i * off, kernel[i], accum, total, centerDepth);
	}

	return accum / total;
}
