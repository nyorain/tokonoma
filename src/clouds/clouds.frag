#version 450

#extension GL_GOOGLE_include_directive : require
#include "noise.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform UBO {
	vec3 camPos;
	float aspect;
	vec3 camDir;
	float fov;
	float time;
} ubo;

layout(set = 0, binding = 1) uniform sampler3D noiseTex;

const vec3 sunDir = normalize(vec3(1, -3, 0.2));
const vec3 sunColor = 5 * vec3(1.0, 0.9, 0.8); // not physically based

// generates the ray direction for this pixel in world space using
// inUV and the camera projection information in ubo
vec3 rayWorldDir() {
	vec2 uv = 2 * inUV - 1;
	uv.y *= -1;

	const vec3 up = vec3(0, 1, 0);
	vec3 dir = normalize(ubo.camDir);
	vec3 x = normalize(cross(dir, up));
	vec3 y = cross(x, dir);

	float maxy = tan(ubo.fov / 2);
	uv *= vec2(maxy * ubo.aspect, maxy);

	return normalize(dir + uv.x * x + uv.y * y);
}

float fbm(vec3 pos) {
	// return texture(noiseTex, pos).r +
	// 	0.5 * texture(noiseTex, 2 * pos).r +
	// 	0.25 * texture(noiseTex, 4 * pos).r;

	return texture(noiseTex, pos).g;
}

const vec3 worldMin = vec3(-4.0, -0.5, -4.0);
const vec3 worldMax = vec3(4.0, 0.5, 4.0);

float world(vec3 pos) {
	if(clamp(pos, worldMin, worldMax) == pos) {
		float heps = 0.1;
		float hf = smoothstep(worldMin.y, worldMin.y + heps, pos.y);
		hf *= (1 - smoothstep(worldMax.y - heps, worldMax.y, pos.y));

		pos *= 0.2;
		float v = fbm(pos);

		// float off = -1.5 * texture(noiseTex, 3.1 * pos).b;
		float off = -0.75 - 0.5 * texture(noiseTex, 4 * pos).b;
		// float off = -1.0 - 0.1 * texture(noiseTex, 4 * pos).b;
		// float off = -1.0 - 0.1 * texture(noiseTex, 4 * pos).b;
		// float off = -1.5;
		off -= (1 - hf) * 0.5;

		v = 10 * (v + off);
		return max(v, 0.0);
	}

	return 0.0;
}

float extinction(float density) {
	return 0.3 * density;
}

float luminance(float density) {
	return 1.0 * density;
}

// Henyey-Greenstein
float phase(float cosTheta, float g) {
	float fac = .079577471545; // 1 / (4 * pi), normalization
	float gg = g * g;
	return fac * (1 - gg) / (pow(1 + gg - 2 * g * cosTheta, 1.5));
}

// volumetric visibility (1 - shadow)
float volVis(vec3 ro) {
	const uint nSteps = 5u;
	const float step = 0.4;

	float t = 0.0;
	// t += (0.0 + random(gl_FragCoord.xyx + ro  + ubo.time)) * step;
	t += 0.1 * step;

	float light = 1.0;
	for(uint i = 0u; i < nSteps; ++i) {
		vec3 pos = ro - t * sunDir;
		float ext = extinction(world(pos));
		light *= exp(-ext * step);
	}

	return light;
}

void main() {
	vec3 ro = ubo.camPos;
	vec3 rd = rayWorldDir();

	const vec3 bg = vec3(0.2, 0.2, 0.4);
	const float minStep = 0.01;
	float step = minStep;

	float t = 0.0;
	t += random(gl_FragCoord.xy + ubo.time) * step;

	float inScatter = 0.0;
	float transmittance = 1.0;

	if(rd.y > 0.0 && ro.y < worldMin.y) {
		float t = (worldMin.y - ro.y) / rd.y;
		ro += t * rd;
	} else if(rd.y < 0.0 && ro.y > worldMax.y) {
		float t = (worldMax.y - ro.y) / rd.y;
		ro += t * rd;
	}

	vec3 diff = ro - ubo.camPos;
	if(dot(diff, diff) > 100.0) {
		outFragColor = vec4(bg, 1.f);
		return;
	}

	bool bigStepping = false;
	for(uint i = 0u; i < 100; ++i) {
		vec3 pos = ro + t * rd;
		if(pos.y > worldMax.y && rd.y > 0 ||
				pos.y < worldMin.y && rd.y < 0) {
			break;
		}

		float w = world(pos);

		if(abs(w) > 0.0) {
			if(bigStepping) {
				bigStepping = false;
				t -= 4.0 * step;
				vec3 pos = ro + t * rd;
				float w = world(pos);
			}

			float it = exp(-w * step); // transmittance of integrated step
			float extinction = extinction(w);
			float lum = luminance(w) * volVis(pos);

			// analytical integral of (e^(-extinction * x) * lum dx)
			// over the current step. Taken from the frostbite paper
			// ([1] in clouds.cpp)
			float integ = (lum - lum * it) / extinction;

			inScatter += transmittance * integ;
			transmittance *= it;
		} else {
			bigStepping = true;
		}

		step = max(minStep, 0.04 * t);
		t += step * (bigStepping ? 4.0 : 1.0);
	}

	float cosTheta = dot(rd, -sunDir);

	const float g = 0.2f;
	float p = phase(cosTheta, g);

	// as in the frostbite theta: allow front and backscattering
	// float p = mix(phase(cosTheta, 0.6f), phase(cosTheta, -0.1f), 0.2);
	vec3 light = inScatter * p * sunColor;

	light += transmittance * bg;

	outFragColor = vec4(vec3(light), 1.0);
}
