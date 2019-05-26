#version 450

#extension GL_GOOGLE_include_directive : enable
#include "fxaa.glsl"
#include "scene.glsl"

const uint flagFXAA = 1 << 1;
const uint tonemapClamp = 0u;
const uint tonemapReinhard = 1u;
const uint tonemapUncharted2 = 2u;
const uint tonemapACES = 3u;
const uint tonemapHeijlRichard = 4u;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform Params {
	uint flags;
	uint tonemap;
	float exposure;
} params;


// http://filmicworlds.com/blog/filmic-tonemapping-operators/
// has a nice preview of different tonemapping operators
vec3 uncharted2map(vec3 x) {
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

vec3 uncharted2tonemap(vec3 x) {
	const float W = 11.2; // whitescale
	x = uncharted2map(x);
	return x * (1.f / uncharted2map(vec3(W)));
}

// Hejl Richard tone map
// http://filmicworlds.com/blog/filmic-tonemapping-operators/
vec3 hejlRichardTonemap(vec3 color) {
    color = max(vec3(0.0), color - vec3(0.004));
    return pow((color*(6.2*color+.5))/(color*(6.2*color+1.7)+0.06), vec3(2.2));
}

// ACES tone map
// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
vec3 acesTonemap(vec3 color) {
    const float A = 2.51;
    const float B = 0.03;
    const float C = 2.43;
    const float D = 0.59;
    const float E = 0.14;
    return clamp((color * (A * color + B)) / (color * (C * color + D) + E), 0.0, 1.0);
}

vec3 tonemap(vec3 x) {
	switch(params.tonemap) {
		case tonemapClamp: return clamp(x, 0.0, 1.0);
		case tonemapReinhard: return vec3(1.0) - exp(-x);
		case tonemapUncharted2: return uncharted2tonemap(x);
		case tonemapACES: return acesTonemap(x);
		case tonemapHeijlRichard: return hejlRichardTonemap(x);
		default: return vec3(0.0); // invalid
	}
}

void main() {
	vec4 color;
	if((params.flags & flagFXAA) != 0) {
		color = fxaa(colorTex, gl_FragCoord.xy);
	} else {
		color = texture(colorTex, uv);
	}

	vec2 texelSize = 1.0 / textureSize(colorTex, 0);

	// mark center
	vec2 dist = textureSize(colorTex, 0) * abs(uv - vec2(0.5));	
	if(max(dist.x, dist.y) < 2) {
		color.rgb = 1 - color.rgb;
	}

	fragColor = vec4(tonemap(color.rgb * params.exposure), 1.0);
	// fragColor = vec4(tonemap(color.rgb), 1.0);
}
