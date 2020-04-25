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
	scatter += textureOffset(scatterTex, uv, ivec2(0, 1));
	scatter += textureOffset(scatterTex, uv, ivec2(1, 0));
	scatter += textureOffset(scatterTex, uv, ivec2(0, -1));
	scatter += textureOffset(scatterTex, uv, ivec2(-1, 0));
	scatter /= 5.f;

	vec3 sum = color.rgb + 0.5 * scatter.rgb;
	sum = 1 - exp(-exposure * sum);

	fragColor = vec4(sum, 1.0);
}
