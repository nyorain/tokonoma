#version 460

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D inTex;

const float exposure = 1.f;

void main() {
	// sharpening
	vec3 original = texture(inTex, uv).rgb;

	float fac = 0.1;
	vec3 sharp = (1 + fac * 4) * original;
	sharp += -fac * textureOffset(inTex, uv, ivec2(1, 0)).rgb;
	sharp += -fac * textureOffset(inTex, uv, ivec2(0, 1)).rgb;
	sharp += -fac * textureOffset(inTex, uv, ivec2(-1, 0)).rgb;
	sharp += -fac * textureOffset(inTex, uv, ivec2(0, -1)).rgb;
 
	vec3 color = sharp;

	// simple tonemap
	fragColor = vec4(1.0 - exp(-exposure * color), 1.0);
}
