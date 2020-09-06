#version 450
#extension GL_GOOGLE_include_directive : require

#include "../spec.glsl"
#include "math.glsl"
#include "color.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 fragColor;

// henyey-greenstein
float phase(float nu, float g) {
	float gg = g * g;
	const float fac = 0.07957747154;
	return fac * ((1 - gg)) / (pow(1 + gg - 2 * g * nu, 1.5f));
}
	
float random(vec2 v) {
	float a = 43758.5453;
	float b = 12.9898;
	float c = 78.233;
	float dt = dot(v, vec2(b, c));
	float sn = mod(dt, 3.14);
	return fract(sin(sn) * a);
}

float linearStep(float low, float high, float t) {
	return clamp((t - low) / (high - low), 0, 1);
}

vec4 sunColors[] = {
	{0.4, 		0.447059,	 0.8,		0.002981},
	{0.384314, 	0.556863,	 0.603922,	0.134128},
	{0.447059, 	0.592157,	 0.654902,	0.171386},
	{0.921569, 	0.54902,	 0.137255,	0.265276},
	{0.992157, 	0.901961,	 0.784314,	0.314456},
	{0.921569, 	0.882353,	 0.839216,	0.499255},
	{0.984314, 	0.819608,	 0.603922,	0.658717},
	{0.984314, 	0.576471,	 0.439216,	0.710879},
	{0.909804, 	0.392157,	 0.219608,	0.751118},
	{0.454902, 	0.34902,	 0.27451,	0.804768},
	{0.219608, 	0.294118,	 0.494118,	0.892698},
};

vec4 ambientColors[] = {
	{0.329412,	0.521569,	0.721569,	0.00149},
 	{0.286275,	0.419608,	0.501961,	0.141579},
 	{0.376471,	0.537255,	0.658824,	0.202683},
 	{0.737255,	0.658824,	0.721569,	0.304024},
 	{0.537255,	0.584314,	0.658824,	0.445604},
 	{0.721569,	0.776471,	0.839216,	0.576751},
 	{0.694118,	0.701961,	0.737255,	0.658718},
	{0.603922,	0.603922,	0.603922,	0.76304},
	{0.576471,	0.529412,	0.666667,	0.81073},
 	{0.309804,	0.509804,	0.74902,	0.886736},
 	{0.329412,	0.521569,	0.721569,	0.998509},
};

vec3 sunColor(float t) {
	for(uint i = 1u; i < sunColors.length(); ++i) {
		vec4 color = sunColors[i];
		if(t < color.a) {
			vec4 prev = sunColors[i - 1];
			float fac = smoothstep(prev.a, color.a, t);
			return mix(prev.rgb, color.rgb, fac);
		}
	}

	// TODO
	return vec3(1, 0, 1);
}

vec3 ambientColor(float t) {
	for(uint i = 1u; i < ambientColors.length(); ++i) {
		vec4 color = ambientColors[i];
		if(t < color.a) {
			vec4 prev = ambientColors[i - 1];
			float fac = smoothstep(prev.a, color.a, t);
			return mix(prev.rgb, color.rgb, fac);
		}
	}

	// TODO
	return vec3(1, 0, 1);
}

vec3 skyColor(vec3 rayDir, float dayTimeN) {
	const float dayTime = 2 * pi * dayTimeN;
	const vec3 toLight = vec3(sin(dayTime), -cos(dayTime), 0.0);

	const vec3 lightColor = sunColor(dayTimeN);
	const vec3 ambientColor = ambientColor(dayTimeN);

	const float nu = dot(rayDir, toLight);
	vec3 light = vec3(0.0);
	light += phase(nu, 0.8) * lightColor;
	light += 2 * phase(nu, 0.60) * ambientColor * lightColor;
	light += 4 * phase(nu, 0.0) * ambientColor;

	return light;
}

void main() {
	vec2 uv = 2 * inUV - 1;
	uv.y *= -1;

	const vec3 up = vec3(0, 1, 0);
	const vec3 dir = normalize(ubo.camDir);
	const vec3 x = normalize(cross(dir, up));
	const vec3 y = cross(x, dir);

	const float maxy = tan(ubo.fov / 2);
	uv *= vec2(maxy * ubo.aspect, maxy);

	const vec3 rayStart = ubo.camPos;
	const vec3 rayDir = normalize(dir + uv.x * x + uv.y * y);

	const float dayTimeN = fract(0.01 * ubo.effect);
	vec3 light = skyColor(rayDir, dayTimeN);

	// tonemap
	light = 1 - exp(-light);

	// dithering
	float rnd1 = random(gl_FragCoord.xy + 0.17);
	float rnd2 = random(gl_FragCoord.xy + 0.85);
	float dither = (rnd1 + rnd2) - 0.5;
	light = toLinearCheap(toNonlinearCheap(light) + dither / 255.f);
	// light = toLinear(toNonlinear(light) + dither / 255.f);
	// light += dither / 255.f;
	fragColor = vec4(light, 1.0);
}
