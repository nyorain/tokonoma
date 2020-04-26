#version 460

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform sampler2D scatterTex;

const float exposure = 1.f;

void main() {
	vec4 color = texture(colorTex, uv);

	// blur it a little bit to get rid of dithering
	vec4 scatter = texture(scatterTex, uv);

#define OFF 1
	scatter += textureOffset(scatterTex, uv, ivec2(0, OFF));
	scatter += textureOffset(scatterTex, uv, ivec2(OFF, 0));
	scatter += textureOffset(scatterTex, uv, ivec2(OFF, OFF));
	scatter += textureOffset(scatterTex, uv, ivec2(-OFF, OFF));
	scatter += textureOffset(scatterTex, uv, ivec2(OFF, -OFF));
	scatter += textureOffset(scatterTex, uv, ivec2(0, -OFF));
	scatter += textureOffset(scatterTex, uv, ivec2(-OFF, 0));
	scatter += textureOffset(scatterTex, uv, ivec2(-OFF, -OFF));
	scatter /= 9.f;

	vec3 sum = color.rgb + scatter.rgb;
	sum = 1 - exp(-exposure * sum);

	fragColor = vec4(sum, 1.0);
}
