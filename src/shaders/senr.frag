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
} scene;

layout(set = 1, binding = 0, row_major) uniform Model {
	mat4 _matrix;
	mat4 _normal;
	vec4 color;
	vec2 faceSize;
	uint id;
} model;

layout(set = 1, binding = 1) uniform usampler2D light;

// TODO: as ubo
const vec3 lightPos = vec3(0, 1.8, 0);
const int filterRange = 2;
const vec2 faceSize = vec2(1024, 1024);

// TODO: many values redundant, think about better storage
//  only 6 values needed i guess: [0][0], [0][1], [0][2], [1][1], [1][2], [2][2]
// XXX: problem with guassian blue (implemented below): we lose hard
// edges where we want them. Guess we need another way to remove noise
float coeffs[5][5] = {
	{0.003765,	0.015019,	0.023792,	0.015019,	0.003765},
	{0.015019,	0.059912,	0.094907,	0.059912,	0.015019},
	{0.023792,	0.094907,	0.150342,	0.094907,	0.023792},
	{0.015019,	0.059912,	0.094907,	0.059912,	0.015019},
	{0.003765,	0.015019,	0.023792,	0.015019,	0.003765},
};

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
	uv *= model.faceSize;
	uv.y += model.id * model.faceSize.y;
	uv.x += face * model.faceSize.x;

	vec2 texelSize = model.faceSize / faceSize;
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
	uv *= model.faceSize;
	uv.y += model.id * model.faceSize.y;
	uv.x += face * model.faceSize.x;
	return unpackUnorm4x8(texture(light, uv).r).rgb;

	/*
	// TODO...
	vec2 texelSize = model.faceSize / faceSize;

	// TODO: should probably be gaussian blur sometehing
	vec3 sum = vec3(0);
	// int count = (2 * filterRange + 1) * (2 * filterRange + 1);
	for(int x = -filterRange; x <= filterRange; ++x) {
	for(int y = -filterRange; y <= filterRange; ++y) {
	float c = coeffs[filterRange + y][filterRange + x];
	vec2 ouv = uv + texelSize * vec2(x, y);
	sum += c * unpackUnorm4x8(texture(light, ouv).r).rgb;
	}
	}

	// return sum / count;
	return sum;
	 */
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
