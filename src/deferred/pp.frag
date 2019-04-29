#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

const uint flagSSAO = (1u << 0u);
const uint flagScattering = (1u << 1u);
const uint flagSSR = (1u << 2u);

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 proj;
	mat4 invProj;
	vec3 viewPos;
	float nearPlane;
	float farPlane;
	uint mode;
} scene;

// layout(set = 1, binding = 0, input_attachment_index = 0)
// 	uniform subpassInput inLight;
layout(set = 1, binding = 0) uniform sampler2D lightTex;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inAlbedo;
layout(set = 1, binding = 2) uniform UBO {
	vec3 scatterLightColor;
	uint tonemap;
	float aoFactor;
	float ssaoPow;
	float exposure;
	uint flags;
} ubo;
layout(set = 1, binding = 3) uniform sampler2D ssaoTex;
layout(set = 1, binding = 4) uniform sampler2D depthTex;
layout(set = 1, binding = 5) uniform sampler2D scatterTex;
layout(set = 1, binding = 6) uniform sampler2D normalTex;
layout(set = 1, binding = 7) uniform sampler2D emissionTex;

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

// completely self developed ssr that uses the depth difference sign
vec3 ssr2(vec3 viewPos, vec3 fragPos, vec3 normal) {
	vec2 px = 1 / textureSize(depthTex, 0); // size of 1 pixel
	vec3 v = normalize(fragPos - viewPos);
	vec3 reflDir = reflect(v, normal);
	vec3 pos = fragPos;
	float fac = 1 - dot(-v, normal);

	const float near = scene.nearPlane;
	const float far = scene.farPlane;
	const uint steps = 16u;
	float stepSize = 0.1;
	const float bepx = 1; // binary end condition in pixels

	for(uint i = 0u; i < steps; ++i) {
		vec3 opos = pos;
		pos += stepSize * reflDir;
		vec3 uv = sceneMap(scene.proj, pos);
		if(uv != clamp(uv, 0.0, 1.0)) {
			return vec3(0.0);
		}

		float depth = textureLod(depthTex, uv.xy, 0).r;
		float z = depthtoz(uv.z, near, far);
		float sampleZ = depthtoz(depth, near, far);
		if(sampleZ < z) { // we are behind a surface; have hit someething
			// we know the segment where we hit now; find a more precise
			// hit now using binary search
			// TODO: also break when we don't move 1 pixel
			// per iteration anymore. Can be determined using the change
			// in uv (in relation to textureSize of depthTex).
			const float bsteps = 8u;
			for(uint i = 0; i < bsteps; ++i) {
				vec3 mid = 0.5 * (opos + pos);
				uv = sceneMap(scene.proj, mid);
				// if(uv != clamp(uv, 0.0, 1.0)) { // TODO: needed?
				// 	return vec3(0.0);
				// }

				float depth = textureLod(depthTex, uv.xy, 0).r;
				float z = depthtoz(uv.z, near, far);
				float sampleZ = depthtoz(depth, near, far);
				if(sampleZ < z) {
					pos = mid;
				} else {
					opos = mid;
				}
			}

			// check normal. if dot is greater 0 we have hit a backside
			vec3 sampleN = decodeNormal(texture(normalTex, uv.xy).xy);
			if(dot(sampleN, reflDir) > 0) {
				return vec3(0.0);
			}

			vec2 dist = 1 - 2 * abs(vec2(0.5, 0.5) - uv.xy);
			fac *= dist.x * dist.y;
			return fac * texture(lightTex, uv.xy).rgb;
		}

		stepSize *= 2;
	}

	return vec3(0.0);
}

// TODO: better blurring/filter for ssao/scattering
//  or move at least light scattering to light shader? that would
//  allow it for multiple light sources (using natural hdr) as
//  well as always using the correct light color and attenuation
//  and such
void main() {
	// vec4 color = subpassLoad(inLight);
	vec4 color = texture(lightTex, uv);
	if(scene.mode >= 1 && scene.mode <= 6) {
		// light shader rendered a debug view, don't modify
		// it in any way. Just forward
		fragColor = vec4(color.rgb, 1.0);
		return;
	}

	// scattering
	if((ubo.flags & flagScattering) != 0) {
		float scatter = 0.f;
		int range = 1;
		vec2 texelSize = 1.f / textureSize(scatterTex, 0);
		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				vec2 off = texelSize * vec2(x, y);
				scatter += texture(scatterTex, uv + off).r;
			}
		}

		int total = ((2 * range + 1) * (2 * range + 1));
		scatter /= total;
		vec3 scatterColor = scatter * ubo.scatterLightColor;
		if(scene.mode == 7) {
			fragColor = vec4(scatterColor, 1.0);
			return;
		}

		color.rgb += scatterColor;
	}

	// ao
	vec4 albedo = subpassLoad(inAlbedo);
	float ao = ubo.aoFactor * albedo.w;

	if((ubo.flags & flagSSAO) != 0) {
		const float near = scene.nearPlane;
		const float far = scene.farPlane;

		float ssao = 0.f;
		int range = 2;
		vec2 texelSize = 1.f / textureSize(ssaoTex, 0);
		float depth = textureLod(depthTex, uv, 0).r;
		float z = depthtoz(depth, near, far);
		float total = 0.f;

		// TODO: what kind of filter to use?
		// without gaussian it's a simple box filter
		// const float kernel[5][5] = {
		// 	{0.003765,	0.015019,	0.023792,	0.015019,	0.003765},
		// 	{0.015019,	0.059912,	0.094907,	0.059912,	0.015019},
		// 	{0.023792,	0.094907,	0.150342,	0.094907,	0.023792},
		// 	{0.015019,	0.059912,	0.094907,	0.059912,	0.015019},
		// 	{0.003765,	0.015019,	0.023792,	0.015019,	0.003765},
		// };

		for(int x = -range; x <= range; ++x) {
			for(int y = -range; y <= range; ++y) {
				vec2 off = texelSize * vec2(x, y); // spaced
				vec2 uvo = clamp(uv + off, 0.0, 1.0);
				float sampleDepth = textureLod(depthTex, uvo, 0.0).r;
				float samplez = depthtoz(sampleDepth, near, far);
				float sampleAo = textureLod(ssaoTex, uvo, 0.0).r;

				// make sure to not blur over edges here
				float fac = smoothstep(0.0, 1.0, 1 - 20 * abs(z - samplez));
				// fac *= kernel[x + range][y + range];
				ssao += fac * sampleAo;
				total += fac;
			}
		}

		// w component of albedo contains texture-based ao factor
		ssao /= total;
		ao *= pow(ssao, ubo.ssaoPow);

		// no blur variant
		// ao *= textureLod(ssaoTex, uv, 0.0).r;
	}

	vec3 aoColor = ao * albedo.rgb;
	if(scene.mode == 8) {
		fragColor = vec4(vec3(ao), 1.0);
		return;
	} else if(scene.mode == 9) {
		fragColor = vec4(aoColor, 1.0);
		return;
	}

	color.rgb += aoColor;

	// ssr
	// TODO: better to do ssr in another pass so it can include ao
	// and light scattering, which is not possible like this.
	// Also move tonemapping in that pass then; basically split
	// post processing into combining and true post processing.
	// The combine pass should also contain the bloom addition.
	if((ubo.flags & flagSSR) != 0) {
		// reconstruct position
		float depth = textureLod(depthTex, uv, 0).r;
		if(depth != 1.f) { // otherwise nothing was rendererd here
			vec2 suv = 2 * uv - 1;
			suv.y *= -1.f; // flip y
			vec4 pos4 = scene.invProj * vec4(suv, depth, 1.0);
			vec3 fragPos = pos4.xyz / pos4.w;
			vec3 normal = decodeNormal(texture(normalTex, uv).xy);
			// color.rgb += ssr(scene.viewPos, fragPos, normal);
			color.rgb += ssr2(scene.viewPos, fragPos, normal);
		}
	}

	// add bloom
	// TODO: blur! in extra pass(es), downscaled
	color.rgb += 4 * texture(emissionTex, uv).rgb;

	fragColor = vec4(tonemap(color.rgb), 1.0);
}
