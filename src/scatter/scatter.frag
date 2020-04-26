#version 460

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) noperspective in vec2 uv; // screen space
layout(location = 0) out vec4 outScatter;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _vp; // view and pojection
	vec3 viewPos;
	float _;
	mat4 vpInv;
} scene;

layout(set = 1, binding = 0, row_major) uniform PointLightBuf {
	PointLight light;
};

layout(set = 1, binding = 1) uniform samplerCubeShadow shadowCube;
layout(set = 2, binding = 0) uniform sampler2D depthTex;

float lightStrength(vec3 worldPos) {
	float shadow = pointShadow(shadowCube, light.pos, light.radius, worldPos);
	float att = defaultAttenuation(distance(worldPos, light.pos), light.radius);
	return shadow * att;
}

// From deferred: scatter.glsl
const float ditherPattern[4][4] = {
	{ 0.0f, 0.5f, 0.125f, 0.625f},
	{ 0.75f, 0.22f, 0.875f, 0.375f},
	{ 0.1875f, 0.6875f, 0.0625f, 0.5625},
	{ 0.9375f, 0.4375f, 0.8125f, 0.3125}};

// - viewPos: camera position in world space
// - pos: sampled position for the given pixel (mapped uv + depth buffer)
// - pixel: current pixel (integer, not normalized)
// To work, the shadowMap(worldPos) function has to be defined in the
// calling shader.
float lightScatterShadow(vec3 viewPos, vec3 pos, vec2 pixel) {
	// first attempt at shadow-map based light scattering
	// http://www.alexandre-pestana.com/volumetric-lights/
	// TODO: here we probably really profit from different mipmaps
	// or some other optimizations...
	vec3 rayStart = viewPos;
	vec3 rayEnd = pos;
	vec3 ray = rayEnd - rayStart;

	float rayLength = length(ray);
	vec3 rayDir = ray / rayLength;
	rayLength = min(rayLength, 20.f);
	ray = rayDir * rayLength;

	const uint steps = 30u;
	vec3 step = ray / steps;

	float accum = 0.0;
	vec3 ipos = rayStart;

	// random dithering, we smooth it out later on
	vec2 ppixel = mod(pixel, vec2(4, 4));
	float ditherValue = ditherPattern[int(ppixel.x)][int(ppixel.y)];
	ipos.xyz += ditherValue * step.xyz;

	// TODO: calculate out-scatter as well?
	for(uint i = 0u; i < steps; ++i) {
		accum += lightStrength(ipos);
		ipos += step;
	}

	return 0.5 * accum * length(step);
}

void main() {
	float depth = texture(depthTex, uv).r;
	vec3 pos = reconstructWorldPos(uv, scene.vpInv, depth);	

	vec2 pixel = gl_FragCoord.xy;
	float scatter = lightScatterShadow(scene.viewPos, pos, pixel);
	outScatter = vec4(scatter * light.color, 1.0);
}

