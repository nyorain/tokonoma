#version 450

#extension GL_GOOGLE_include_directive : enable
#include "geometry.glsl"

layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

const uint TypeForward = 1u;
const uint TypeVelocity = 2u;
const uint TypeLightDensity = 3u;
layout(push_constant) uniform Type {
	uint type;
} type;

vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main() {
	// normal
	out_color.a = 1.0;
	vec4 read = texture(tex, in_uv);
	if(type.type == TypeForward) {
		out_color.rgb = read.rgb;
	} else if(type.type == TypeLightDensity) {
		vec2 light = vec2(0.5, 0.5);
		float radius = 0.1;
		// vec2 diff = (in_uv - light) - radius;
		vec2 diff = (in_uv - light);
		// vec2 off = diff * radius / length(diff);
		// diff -= off;
		float l = max(length(diff), 0);
		int samples = int(l * 250);
		// int samples = 100;
		// const float fac = 0.1;
		// const float fac = 1 / samples;
		const float fac = 0.5f;
		float accum = 1.f;
		// vec2 pos = light + off;
		vec2 pos = light;
		for(int i = 0; i < samples; ++i) {
			// accum -= fac * pow(texture(tex, pos).r, 1.5);
			accum -= fac * texture(tex, pos).r;
			pos += diff / samples;
		}

		accum = clamp(accum, 0, 1);
		float lightFac = lightFalloff(light, in_uv, radius, 1.0);

		out_color.rgb = vec3(texture(tex, in_uv).r);
		out_color.rgb += vec3(1, 1, 0.5) * vec3(lightFac * accum);

		// #2: to make smoke hide somke behind it
		// out_color.rgb += vec3(lightFac);
		// out_color.rgb *= vec3(accum);
	} else if(type.type == TypeVelocity) {
		// #2 velocity using hsv
		vec2 vel = texture(tex, in_uv).xy;
		const float pi = 3.1415; // good enough
		float angle = (atan(vel.y, vel.x) + pi) / (2 * pi); // which color
		float s = 25 * (abs(vel.x) + abs(vel.y)); // how much color
		float v = 16 * dot(vel, vel); // how much light
		out_color.rgb = hsv2rgb(vec3(angle, s, v));
	}

	// old normalize (e.g. velocity)
	// out_color.b = 0.2 * length(out_color.rg);
	// out_color.rg *= 0.5;
	// out_color.rg += 0.5;
	// out_color.rg *= 2;
}
