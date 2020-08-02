#version 450
#extension GL_GOOGLE_include_directive : enable

#include "math.glsl"
#include "geometry.glsl"
#include "noise.glsl"
#include "snoise.glsl"

layout(location = 0) in vec2 inuv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;
	uint effect;
} ubo;

float mfbm(vec2 st) {
	// const mat2 mtx = mat2(6.4, fbm(vec2(cos(0.3 * ubo.time), 7 * sin(0.3241 * ubo.time))), 1, 2);
	const mat2 mtx = mat2( 1, 0, 0, 1);

	float sum = 0.f;
	float lacunarity = 1.1;
	float gain = 0.5;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < FBM_OCTAVES; ++i) {
		// sum += amp * valueNoise(2 * random(mod * amp) + mod * st);
		sum += amp * gradientNoise(st);
		// sum += amp * FBM_NOISE(mod * st);
		// mod *= lacunarity;
		amp *= gain;
		// st = lacunarity * mtx * st;
		st = cos(ubo.time) * fbm(sin(ubo.time + fbm(amp * st) * ubo.time) * sum * st) * st;
	}

	return sum;
}

// voronoi
float vfbm(vec2 st) {
	float sum = 0.f;
	float lacunarity = 2.0;
	float gain = 0.5;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < FBM_OCTAVES; ++i) {
		sum += amp * (1 - voronoiNoise(mod * st));
		mod *= lacunarity;
		amp *= gain;
		st = lacunarity * st;
	}

	return sum;
}

float tfbm(vec2 st) {
	float sum = 0.f;
	float lacunarity = 2.0;
	float gain = 0.3;

	float amp = 0.5f; // ampliture
	float mod = 1.f; // modulation
	for(int i = 0; i < 4; ++i) {
		sum += amp * gradientNoise(mod * st);
		mod *= lacunarity;
		amp *= gain;
		st = lacunarity * st;
	}

	return sum;
}

void main() {
	vec2 uv = 2 * inuv - 1;
	uv = 10 * inuv;

	float d = 0.f;
	vec3 rgb = vec3(1.0);
	int counter = 0;
	if(ubo.effect == counter++) {
		d = valueNoise(100 * ubo.mpos + uv);
	} else if(ubo.effect == counter++) {
		d = gradientNoise(100 * ubo.mpos + uv);
	} else if(ubo.effect == counter++) {
		d = voronoiNoise(100 * ubo.mpos + uv);
	} else if(ubo.effect == counter++) {
		d = 0.75 * voronoiNoise2(100 * ubo.mpos + uv);
	} else if(ubo.effect == counter++) {
		d = 0.5 * voronoiNoise3(100 * ubo.mpos + uv);
	} else if(ubo.effect == counter++) {
		d = fbm(uv);
	} else if(ubo.effect == counter++) {
		d = vfbm(uv);
	} else if(ubo.effect == counter++) {
		float v1 = fbm(uv - vec2(cos(ubo.time), sin(ubo.time)));
		float v2 = fbm(v1 + uv);
		d = v2;
		rgb.r = v1;
		rgb.g = v2;
		rgb.b = v1 * v2;
	} else if(ubo.effect == counter++) {
		d = fbm(2 * uv + fbm(uv + vec2(1, 1) * pow(fbm(uv), 2)));
		d = sin(fbm(d * vec2(sin(uv.x), cos(uv.y))));
		d = fbm(150 * vec2(sin(d), cos(d)) * fbm(vec2(d, -d)));
	} else if(ubo.effect == counter++) {
		d = fbm(uv + mfbm(uv + fbm(uv)));
	} else if(ubo.effect == counter++) {
		uv += 30 * ubo.mpos;

		// simple first try at a universe background shader
		// (ideally to be applied to a 3D skybox)
		// things to try:
		// - fbm: combine color and cloud-like structures
		// - raymarching through 3D noise
		d = max(4 * pow(snoise(5 + 25 * vec3(-uv, 0.0)), 60), 0);
		d += (0.8 + 0.2 * snoise(vec3(10 * uv, ubo.time))) *
			max(50 * pow(0.5 + 0.5 * snoise(5 + 10 * vec3(uv, 0.0)), 100), 0);
		d += (0.8 + 0.3 * snoise(vec3(6 * uv, ubo.time))) *
			max(10 * pow(0.5 + 0.5 * snoise(-100 + 6 * vec3(uv.x, 0.0, uv.y)), 50), 0);

		float u = 0.5 + 0.5 * snoise(vec3(3 * uv, 0));
		u = pow(u, 2);
		rgb = 4 * u * vec3(1 - u, 1 - u, u);
		outcol = vec4(d * rgb, 1.0);

		float f = 0.5 + 0.5 * snoise(vec3(0.1 * uv, 0));
		outcol.rgb += 0.05 * 
			pow((0.5 + 0.5 * snoise(vec3(0.2 * f, 10 + 0.1 * uv))), 0.5) *
			vec3(f, 1 - f, 1 - f);

		float g = 0.5 + 0.5 * snoise(vec3(-0.03 * uv, 0));
		outcol.rgb += 0.02 * 
			pow((0.5 + 0.5 * snoise(0.3 * vec3(g, -g, g))), 0.9) *
			vec3(1 - g, g, 0.0);

		uint count = 20;
		for(uint i = 1; i < 20; ++i) {
			outcol.rgb += 0.1 * snoise(vec3(uv, i)) / (count * i);
		}

		float step = 3.4;
		for(uint i = 1; i < 20; ++i) {
			uv += 0.7 * ubo.mpos;
			outcol.rgb += (0.8 + 0.5 * snoise(vec3(11 * uv, ubo.time))) * 
				max(20 * pow(snoise(vec3(11 * uv, i * step)), 30) / count, 0.0);
		}

		for(uint i = 1; i < 20; ++i) {
			uv -= 0.2 * ubo.mpos;
			outcol.b += max(0.1 * pow(snoise(vec3(0.02 * uv, i * 5.4)), 4) / count, 0.0);
		}

		uv -= 3 * ubo.mpos;

		float b = pow(fbm(uv), 3);
		outcol.rgb += 0.1 * fbm(vec2(uv.y * b, uv.x)) * vec3(b, 1 - b, 1 - b);
		outcol.rgb += 0.1 * fbm(vec2(uv.y, b * uv.x)) * vec3(1 - b * b, b * b, b * b);

		float h = 10 * pow(fbm(0.1 * uv), 6);
		// float h = 10 * pow(0.5 + 0.5 * snoise(vec3(0.01 * uv, 0.0 * b)), 8);
		outcol.rgb += 0.2 * max(100 * h * pow(snoise(5 + 9 * vec3(-uv, b)), 20), 0);
		// outcol.rgb += h;

		outcol.rgb = pow(outcol.rgb, vec3(2.2));
		return;
	}

	outcol = vec4(d * rgb, 1.0);
	outcol.rgb = pow(outcol.rgb, vec3(2.2));
}
