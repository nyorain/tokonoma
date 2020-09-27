#include "noise.glsl"
#include "color.glsl"
#include "constants.glsl"
#include "atmosphere.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1, row_major) uniform Scene {
	UboData scene;
};

void main() {
	vec3 color = texture(colorTex, inUV).rgb;

	// tonemap
	float exposure = 1.0;
	color = 1 - exp(-exposure * color);

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
	outFragColor = vec4(toLinearCheap(toNonlinearCheap(color) + dither / 255.f), 1.0);
}
