#version 460

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) noperspective in vec2 uv; // screen space
layout(location = 0) out vec4 outScatter;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _vp; // view and pojection
	mat4 _vpPrev;
	mat4 vpInv;
	vec3 viewPos;
	uint frameCount;
	vec2 _jitter;
	float _near;
	float _far;
} scene;

layout(set = 1, binding = 0, row_major) uniform PointLightBuf {
	PointLight light;
};

layout(set = 1, binding = 1) uniform samplerCubeShadow shadowCube;
layout(set = 2, binding = 0) uniform sampler2D depthTex;
layout(set = 2, binding = 1) uniform sampler2DArray noiseTex;

float phase(float cosTheta, float g) {
	float gg = g * g;

	// from csp atmosphere and gpugems, normalized
	const float fac = 0.11936620731; // 3 / (8 * pi) for normalization
	float cc = cosTheta * cosTheta;
	return fac * ((1 - gg) * (1 + cc)) / ((2 + gg) * pow(1 + gg - 2 * g * cosTheta, 1.5));
}

float scatterStrength(vec3 worldPos) {
	// const float mieG = -0.9;
	// const float mieG = -0.6;
	const float mieG = -0.2;
	float shadow = pointShadow(shadowCube, light.pos, light.radius, worldPos);
	float att = defaultAttenuation(distance(worldPos, light.pos), light.radius);

	vec3 ldir = normalize(worldPos - light.pos);
	vec3 vdir = normalize(worldPos - scene.viewPos);
	float cosTheta = dot(ldir, vdir);

	float scatter = phase(cosTheta, mieG);
	return max(scatter * shadow * att, 0.0);
}

// From deferred: scatter.glsl
const float ditherPattern[4][4] = {
	{ 0.0f, 0.5f, 0.125f, 0.625f},
	{ 0.75f, 0.22f, 0.875f, 0.375f},
	{ 0.1875f, 0.6875f, 0.0625f, 0.5625},
	{ 0.9375f, 0.4375f, 0.8125f, 0.3125}};

int bayer8[8][8] = {
	{ 0, 32, 8, 40, 2, 34, 10, 42}, /* 8x8 Bayer ordered dithering */
	{48, 16, 56, 24, 50, 18, 58, 26}, /* pattern. Each input pixel */
	{12, 44, 4, 36, 14, 46, 6, 38}, /* is scaled to the 0..63 range */
	{60, 28, 52, 20, 62, 30, 54, 22}, /* before looking in this table */
	{ 3, 35, 11, 43, 1, 33, 9, 41}, /* to determine the action. */
	{51, 19, 59, 27, 49, 17, 57, 25},
	{15, 47, 7, 39, 13, 45, 5, 37},
	{63, 31, 55, 23, 61, 29, 53, 21} 
};

// - viewPos: camera position in world space
// - pos: sampled position for the given pixel (mapped uv + depth buffer)
// - pixel: current pixel (integer, not normalized)
// To work, the shadowMap(worldPos) function has to be defined in the
// calling shader.
float lightScatterShadow(vec3 rayStart, vec3 rayEnd, vec2 pixel) {
	// TODO: here we probably really profit from different mipmaps
	// or some other optimizations...
	vec3 ray = rayEnd - rayStart;

	// optional: set maximum ray length
	// float rayLength = length(ray);
	// vec3 rayDir = ray / rayLength;
	// rayLength = min(rayLength, 10.f);
	// ray = rayDir * rayLength;

	const uint steps = 8u;
	vec3 step = ray / steps;

	float accum = 0.0;
	vec3 ipos = rayStart;

	// random dithering, we smooth it out later on
	// vec2 ppixel = mod(pixel, vec2(4, 4));
	// float ditherValue = ditherPattern[int(ppixel.x)][int(ppixel.y)];
	// ipos.xyz += ditherValue * step.xyz;
	
	// TODO: could be improved I guess?
	// TODO: scene.jitter currently normalized to pixelSize...
	// float jit = 0.5 + 0.5 * scene.jitter.x * textureSize(depthTex, 0).x;
	// vec2 jit = 0.5 + 0.5 * scene.jitter * textureSize(depthTex, 0);
	// ppixel = mod(pixel + 8 * jit, vec2(8, 8));
	// ditherValue = (1.f / 64.f) * bayer8[int(ppixel.x)][int(ppixel.y)];

	vec2 puv = mod(pixel, textureSize(noiseTex, 0).xy);
	puv /= textureSize(noiseTex, 0).xy;
	uint layer = scene.frameCount % textureSize(noiseTex, 0).z;
	// uint layer = 0u;
	float ditherValue = texture(noiseTex, vec3(puv, layer)).r;
	ipos.xyz += ditherValue * step.xyz;

	// re-normalize
	// ray = rayEnd - rayStart;
	// step = ray / steps;

	// TODO: calculate out-scatter as well?
	for(uint i = 0u; i < steps; ++i) {
		accum += scatterStrength(ipos);
		ipos += step;
	}

	// TODO: not sure why clamping to 0 here is needed, accum
	// can't get negative. Otherwise we get some black-pixel
	// artefacts. Maybe accum overflows in same cases?
	return max(accum * length(step), 0.0);
}

// ro: ray origin
// rd: ray direction
// cc: sphere center
// cr: sphere radius
// - assumes that rd is normalized
// - returns vec2(-1.0) when there is no intersection.
//   this means the valid solution vec2(-1.0) is not returned correctly.
vec2 raySphere(vec3 ro, vec3 rd, vec3 sc, float sr) {
	// Solution can easily be found using pq-formula
	vec3 oc = ro - sc;
	float p2 = dot(rd, oc); // p/2
	float q = dot(oc, oc) - sr * sr; // q
	float s = p2 * p2 - q; // (p/2)^2 - q
	if(s < 0.0) {
		return vec2(-1.0);
	}

	s = sqrt(s);
	return -p2 + vec2(-s, s);
}

void main() {
	float depth = texture(depthTex, uv).r;
	vec3 pos = reconstructWorldPos(uv, scene.vpInv, depth);	

	vec3 ro = scene.viewPos;
	vec3 rd = normalize(pos - scene.viewPos);
	vec2 is = raySphere(ro, rd, light.pos, light.radius);
	if(is.x < 0.0 && is.y < 0.0) {
		// discard; // no scattering here
		outScatter = vec4(0.0);
		return;
	}

	// manual depth check, we can't use the depth buffer for
	// rendering because we read from it as texture.
	// TODO: maybe doing both is allowed/possible?
	if(dot(rd, pos - ro) < (dot(rd, light.pos - ro) - light.radius)) {
		discard;
	}

	vec3 start = ro + max(0.0, min(is.x, is.y)) * rd;
	vec3 end = ro + max(is.x, is.y) * rd;
	if(distance(pos, light.pos) < light.radius) {
		end = pos;
	}

	vec2 pixel = gl_FragCoord.xy;
	float scatter = lightScatterShadow(start, end, pixel);
	outScatter = vec4(scatter * light.color, 1.0);
}

