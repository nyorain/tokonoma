#version 450
#extension GL_GOOGLE_include_directive : enable

#include "../geometry.glsl"
#include "../noise.glsl"

layout(location = 0) in vec2 inuv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;
} ubo;

float pattern( in vec2 p, out vec2 q, out vec2 r) {
	q.x = fbm(p + vec2(ubo.time,0.0));
	q.y = fbm(p + vec2(5.2,1.3));

	r.x = fbm(p + 4.0*q + vec2(1.7,9.2));
	r.y = fbm(p + 4.0*q + vec2(8.3,2.8));

	return fbm(p + 4.0 * r);
}

void main() {
	vec2 uv = 20 * inuv;
	vec2 q, r;
	float d = pattern(uv, q, r);

	outcol = d * vec4(dot(q, q - r), dot(r, r), dot(q, q), 1.0);
	outcol.rgb = pow(outcol.rgb, vec3(2.2));
}

