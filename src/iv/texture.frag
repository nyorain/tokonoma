#version 450
#extension GL_GOOGLE_include_directive : require

#include "bicubic.glsl"
#include "math.glsl"
#include "noise.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outFragColor;

#ifdef TEX3D
  layout(set = 0, binding = 0) uniform sampler3D tex;
#else // TEX3D
  layout(set = 0, binding = 0) uniform sampler2D tex;
#endif // TEX3D

layout(set = 0, binding = 1) uniform UBO {
	vec2 offset;
	vec2 size;

#ifdef TONEMAP
	float exposure;
#endif // TONEMAP

#ifdef TEX3D
	float depth;
#endif // TEX3D
} ubo;

void main() {
	vec2 uv = ubo.offset + ubo.size * inUV;

#ifdef TEX3D
	vec4 color = 0.5 + 0.05 * texture(tex, vec3(uv, ubo.depth));
#else // TEX3D
	// vec4 color = textureBicubic(tex, uv);
	// vec4 color = textureBicubicCatmull(tex, uv);
	vec4 color = texture(tex, uv);
#endif // TEX3D

#ifdef TONEMAP
	color.rgb = 1.0 - exp(-ubo.exposure * color.rgb);
#endif // TONEMAP
	outFragColor = color;
}

/*
const vec3 colors[] = {
	{1, 0, 0},
	{1, 1, 0},
	{0, 1, 0},
	{0, 1, 1},
	{0, 0, 1},
	{1, 0, 1},
};

void main() {
	vec2 uv = ubo.offset + ubo.size * inUV;

	float index = 0.0;
	index += 8 * gradientNoise(4 * uv);
	// index += 2 * gradientNoise(8 * uv);
	// index += 2 * voronoiNoise(8 * uv);
	// index = 0;

	int id0 = int(floor(index));
	int id1 = id0 + 1;
 
	// vec2 offa = sin(vec2(3.0,7.0)*(id0)); // can replace with any other hash    
    // vec2 offb = sin(vec2(3.0,7.0)*(id1)); // can replace with any other hash
	vec2 offa = random2(vec2(id0, 1 + id0));
	vec2 offb = random2(vec2(id1, 1 + id1));
 
	vec4 color = mix(
		texture(tex, fract(uv + offa)),
		texture(tex, fract(uv + offb)),
		smoothstep(0, 1, fract(index)));

	// int id0 = int(mod(floor(index), colors.length()));
	// int id1 = int(mod(id0 + 1, colors.length()));
	// vec3 color = mix(colors[id0], colors[id1], smoothstep(0, 1, fract(index)));

	// outFragColor = vec4(color, 1.0);
	outFragColor = color;
}
*/
