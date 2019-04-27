#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, input_attachment_index = 0)
	uniform subpassInput inLight;
layout(set = 0, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;
layout(set = 0, binding = 2) uniform UBO {
	vec3 scatterLightColor;
	uint tonemap;
	float ssaoFactor;
	float exposure;
} ubo;
// layout(set = 0, binding = 3) uniform sampler2D ssaoTex;
// layout(set = 0, binding = 4) uniform sampler2D scatterTex;

// http://filmicworlds.com/blog/filmic-tonemapping-operators/
// has a nice preview of different tonemapping operators
// currently using uncharted 2 version
vec3 uncharted2map(vec3 x) {
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

vec3 uncharted2tonemap(vec3 x, float exposure) {
	const float W = 11.2; // whitescale
	x = uncharted2map(x * ubo.exposure);
	return x * (1.f / uncharted2map(vec3(W)));
}

vec3 tonemap(vec3 x) {
	switch(ubo.tonemap) {
		case 0: return x;
		case 1: return vec3(1.0) - exp(-x * ubo.exposure);
		case 2: return uncharted2tonemap(x, ubo.exposure);
		default: return vec3(0.0); // invalid
	}
}

// TODO: better blurring/filter for ssao/scattering
//  or move at least light scattering to light shader? that would
//  allow it for multiple light sources (using natural hdr) as
//  well as always using the correct light color and attenuation
//  and such
void main() {
	vec4 color = subpassLoad(inLight);

	/*
	// scattering
	{
		float scatter = 0.f;
		int range = 1;
		vec2 texelSize = 1.f / textureSize(scatterTex, 0);
		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				vec2 off = texelSize * vec2(x, y);
				scatter += texture(scatterTex, uv + off).a;
			}
		}

		int total = ((2 * range + 1) * (2 * range + 1));
		scatter /= total;
		color.rgb += scatter * ubo.scatterLightColor;
	}
	*/

	/*
	// ssao
	{
		float ao = 0.f;
		int range = 1;
		vec2 texelSize = 1.f / textureSize(ssaoTex, 0);
		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				vec2 off = texelSize * vec2(x, y);
				ao += texture(ssaoTex, uv + off).a;
			}
		}

		int total = ((2 * range + 1) * (2 * range + 1));
		ao /= total;


		float ao = 0.2f;
		// w component contains texture-based ao factor
		vec4 albedo = subpassLoad(inAlbedo);
		// color.rgb += ao * ubo.ssaoFactor * albedo.rgb * albedo.w;
		color.rgb += ao * albedo.rgb;
	}
	*/

	// TODO: simple ao, remove when we have ssao
	float ao = 0.2f * ubo.ssaoFactor;
	vec4 albedo = subpassLoad(inAlbedo);
	color.rgb += ao * albedo.rgb;

	fragColor = vec4(tonemap(color.rgb), 1.0);
}
