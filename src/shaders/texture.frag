#version 450

layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

const uint TypeForward = 1u;
const uint TypeVelocity = 2u;
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
	} else if(type.type == TypeVelocity) {
		// #2 velocity using hsv
		vec2 vel = texture(tex, in_uv).xy;
		const float pi = 3.1415; // good enough
		float angle = (atan(vel.y, vel.x) + pi) / (2 * pi); // which color
		float s = 25 * (abs(vel.x) + abs(vel.y)); // how much color
		float v = 16 * dot(vel, vel); // how much light
		out_color.rgb = hsv2rgb(vec3(angle, s, v));
		out_color.a = 1;
	}

	// old normalize (e.g. velocity)
	// out_color.b = 0.2 * length(out_color.rg);
	// out_color.rg *= 0.5;
	// out_color.rg += 0.5;
	// out_color.rg *= 2;
}
