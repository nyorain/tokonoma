#version 450

#extension GL_GOOGLE_include_directive : enable
#include "geometry.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform UBO {
	vec2 mousePos;
} ubo;

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
		// NOTE: probably best to do in an extra pass with way lower sample
		// count and some noise. Then blur here.

		const float step = 0.001;
		const float radius = 0.01;
		const vec2 light = ubo.mousePos;
		const float densityFac = 20.f;

		// NOTE: alternative: ignore smoke over light
		// vec2 diff = (in_uv - light) - radius;

		vec2 diff = in_uv - light;
		float l = length(diff);
		diff /= l;
		int samples = int(l / step);

		float accum = 1.f;
		vec2 pos = light;
		for(int i = 0; i < samples; ++i) {
			accum -= step * texture(tex, pos).r * densityFac;
			pos += step * diff;
		}

		accum = clamp(accum, 0, 1);
		// float lightFac = lightFalloff(light, in_uv, radius, 2.0,
		// 		vec3(0.1, 10, 20), 0.f, true);
		float lightFac = 1.0;

		// out_color.rgb = vec3(1, 0.9, 0.6) * vec3(lightFac * accum);
		// out_color.rgb -= read.rgb;
		// out_color.rgb = pow(out_color.rgb, vec3(2.2)); // gamma

		// #2: to make smoke hide somke behind it
		out_color.rgb = vec3(lightFac - texture(tex, in_uv).r);
		out_color.rgb *= vec3(accum);
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
