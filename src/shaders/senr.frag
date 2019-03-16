#version 450

#extension GL_GOOGLE_include_directive : enable

#include "util.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inPosm;

layout(location = 0) out vec4 outCol;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	vec3 viewPos;
	uint showLightTex;
	vec2 faceSize; // size (in pixels) of one face
	vec2 atlasSize; // total size (in pixels) of atlas
} scene;

layout(set = 1, binding = 0, row_major) uniform Model {
	mat4 _matrix;
	mat4 _normal;
	vec4 color;
	uint id;
} model;

layout(set = 1, binding = 1) uniform usampler2D light;

// TODO: as ubo
const vec3 lightPos = vec3(0, 1.8, 0);

// TODO: make more sense to apply this filter to the texture instead
// of just using it for displaying. Otherwise e.g. reflections
// are "wrong" (e.g. shadows not blurred)
// filter credit: https://www.shadertoy.com/view/4dfGDH
#define SIGMA 1.25
#define BSIGMA 0.8
#define MSIZE 5

float normpdf(in float x, in float sigma) {
	return 0.39894*exp(-0.5*x*x/(sigma*sigma))/sigma;
}

float normpdf3(in vec3 v, in float sigma) {
	return 0.39894*exp(-0.5*dot(v,v)/(sigma*sigma))/sigma;
}

vec3 lightColorBlurred() {
	vec2 uv;
	int face = cubeFace(inPosm, uv); // uv is returned as [-1,1] here
	uv = 0.5 + 0.5 * uv;
	vec2 faceSize = scene.faceSize / scene.atlasSize;
	uv *= faceSize;
	uv.y += model.id * faceSize.y;
	uv.x += face * faceSize.x;

	vec2 texelSize = faceSize / scene.faceSize;
	vec3 c = unpackUnorm4x8(texture(light, uv).r).rgb;

	const int kSize = (MSIZE-1)/2;
	float kernel[MSIZE];
	vec3 final_color = vec3(0.0);

	//create the 1-D kernel
	float Z = 0.0;
	for(int j = 0; j <= kSize; ++j) {
		kernel[kSize+j] = kernel[kSize-j] = normpdf(float(j), SIGMA);
	}


	vec3 cc;
	float factor;
	float bZ = 1.0/normpdf(0.0, BSIGMA);
	//read out the texels
	for (int i=-kSize; i <= kSize; ++i) {
		for (int j=-kSize; j <= kSize; ++j) {
			vec2 ouv = uv + texelSize * vec2(i, j);
			cc = unpackUnorm4x8(texture(light, ouv).r).rgb;
			factor = normpdf3(cc-c, BSIGMA)*bZ*kernel[kSize+j]*kernel[kSize+i];
			Z += factor;
			final_color += factor*cc;
		}
	}

	return final_color;
}

vec3 lightColorDirect() {
	vec2 uv;
	int face = cubeFace(inPosm, uv); // uv is returned as [-1,1] here
	uv = 0.5 + 0.5 * uv;
	vec2 faceSize = scene.faceSize / scene.atlasSize;
	uv *= faceSize;
	uv.y += model.id * faceSize.y;
	uv.x += face * faceSize.x;
	return unpackUnorm4x8(texture(light, uv).r).rgb;
}

vec3 lightColor() {
	return lightColorBlurred(); // blurred
	// return lightColorDirect(); // not blurred
}

void main() {
	if(scene.showLightTex == 1) {
		vec3 light = lightColor();
		outCol = vec4(model.color.rgb, 1.0) * vec4(light, 1.0);
	} else {
		vec3 normal = normalize(inNormal);
		vec3 objectColor = model.color.rgb;
		float ambientFac = 0.1f;
		float diffuseFac = 0.4f;
		float specularFac = 0.4f;
		float shininess = 32.f;

		// ambient
		vec3 col = vec3(0.0);
		if(scene.showLightTex == 0) {
			col += ambientFac * objectColor;
		}

		// diffuse
		vec3 ldir = normalize(lightPos - inPos);
		col += diffuseFac * objectColor * max(dot(ldir, normal), 0.0);

		// blinn-phong
		vec3 vdir = normalize(scene.viewPos - inPos);
		vec3 h = normalize(vdir + ldir);
		col += specularFac * objectColor * pow(max(dot(normal, h), 0.0), shininess);

		if(scene.showLightTex == 2) {
			vec3 light = lightColor();
			col += light;
		}

		outCol = vec4(col, 1.0);
	}
}
