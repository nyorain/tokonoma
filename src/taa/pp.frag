#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D inTex;
layout(set = 0, binding = 1) uniform UBO {
	float sharpen;
	float exposure;
} ubo;

void main() {
	// sharpening
	vec3 original = texture(inTex, uv).rgb;

	float fac = ubo.sharpen;
	vec3 sharp = (1 + fac * 4) * original;
	sharp += -fac * textureOffset(inTex, uv, ivec2(1, 0)).rgb;
	sharp += -fac * textureOffset(inTex, uv, ivec2(0, 1)).rgb;
	sharp += -fac * textureOffset(inTex, uv, ivec2(-1, 0)).rgb;
	sharp += -fac * textureOffset(inTex, uv, ivec2(0, -1)).rgb;
 
	vec3 color = sharp;

	// simple tonemap
	fragColor = vec4(1.0 - exp(-ubo.exposure * color), 1.0);
}
