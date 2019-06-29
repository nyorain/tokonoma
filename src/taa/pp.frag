#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D inTex;

const float exposure = 1.f;

void main() {
	// sharpening
	vec3 original = texture(inTex, uv).rgb;
	// vec3 sharp = 5 * original;
	// sharp += -textureOffset(inTex, uv, ivec2(1, 0)).rgb;
	// sharp += -textureOffset(inTex, uv, ivec2(0, 1)).rgb;
	// sharp += -textureOffset(inTex, uv, ivec2(-1, 0)).rgb;
	// sharp += -textureOffset(inTex, uv, ivec2(0, -1)).rgb;
// 
	// float fac = 0.2;
	// vec3 color = mix(original, sharp, fac);

	// TODO
	vec3 color = original;

	// simple tonemap
	fragColor = vec4(1.0 - exp(-exposure * color), 1.0);
}
