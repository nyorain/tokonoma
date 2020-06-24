#version 460

#extension GL_GOOGLE_include_directive : require
#include "color.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;

// only used for dithering. Bad function.
float random(vec2 v) {
	float a = 43758.5453;
	float b = 12.9898;
	float c = 78.233;
	float dt = dot(v, vec2(b, c));
	float sn = mod(dt, 3.14);
	return fract(sin(sn) * a);
}

void main() {
	const float exposure = 1.f;

	vec4 col = texture(colorTex, inUV);
	col.rgb = 1 - exp(-exposure * col.rgb);
	fragColor = col;

	// anti-banding dithering
	// important that we do this after conversion to srgb, i.e. on the
	// real, final 8-bit pixel values
	// Also important: no bias here
	// We use a triangular noise distribution for best results.
	// TODO: use blue noise instead for random samples.
	// TODO: tri distr can be achieved more efficiently with a single random
	//  sample, just search shadertoy (+ discussions on best remapping)
	float rnd1 = random(gl_FragCoord.xy + 0.17);
	float rnd2 = random(gl_FragCoord.xy + 0.85);
	float dither = 0.5 * (rnd1 + rnd2) - 0.5;
	fragColor.rgb = toLinearCheap(toNonlinearCheap(fragColor.rgb) + dither / 255.f);
}
