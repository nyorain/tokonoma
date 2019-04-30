#include "noise.glsl"

// Light.flags
const uint lightDir = (1u << 0); // otherwise point
const uint lightPcf = (1u << 1); // use pcf

// MaterialPcr.flags
const uint normalMap = (1u << 0);
const uint doubleSided = (1u << 1);

// struct Light {
// 	vec3 pos; // position for point light, direction of dir light
// 	uint type; // point or dir
// 	vec3 color;
// 	uint pcf;
// };

struct DirLight {
	vec3 color; // w: pcf
	uint flags;
	vec3 dir;
	float _; // unused padding
	mat4 proj; // global -> light space
};

struct PointLight {
	vec3 color;
	uint flags;
	vec3 pos;
	float farPlane;
	vec3 attenuation;
	float _; // padding
	mat4 proj[6]; // global -> cubemap [side i] light space
};

struct MaterialPcr {
	vec4 albedo;
	float roughness;
	float metallic;
	uint flags;
	float alphaCutoff;
	vec3 emission;
};

// returns the z value belonging to the given depth buffer value
// obviously requires the near and far plane values that were used
// in the perspective projection.
// depth expected in standard vulkan range [0,1]
float depthtoz(float depth, float near, float far) {
	return near * far / (far + near - depth * (far - near));
}
float ztodepth(float z, float near, float far) {
	if(z == 1000.f) { // TODO
		return 1.f;
	}
	return (near + far - near * far / z) / (far - near);
}
// NOTE: we could implement an alternative version that uses the projection
// matrix for depthtoz. But we always use the premultiplied
// projectionView matrix and we can't get the information out of that.


// computes shadow for directional light
// returns (1 - shadow), i.e. the light factor
float dirShadow(sampler2DShadow shadowMap, vec3 pos, int range) {
	if(pos.z > 1.0) {
		return 1.0;
	}

	vec2 texelSize = 1.f / textureSize(shadowMap, 0);
	float sum = 0.f;
	for(int x = -range; x <= range; ++x) {
		for(int y = -range; y <= range; ++y) {
			// sampler has builtin comparison
			vec3 off = vec3(texelSize * vec2(x, y),  0);
			sum += texture(shadowMap, pos + off).r;
		}
	}

	float total = ((2 * range + 1) * (2 * range + 1));
	return sum / total;
}

// Calculates the light attentuation based on the distance d and the
// light attenuation parameters
float attenuation(float d, vec3 params) {
	return 1.f / (params.x + params.y * d + params.z * (d * d));
}

// from https://learnopengl.com/Advanced-Lighting/Shadows/Point-Shadows
vec3 pointShadowOffsets[20] = vec3[](
   vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1), 
   vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
   vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
   vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
   vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
);

float pointShadow(samplerCubeShadow shadowCube, vec3 lightPos,
		float lightFarPlane, vec3 fragPos) {
	vec3 dist = fragPos - lightPos;
	float d = length(dist) / lightFarPlane;
	return texture(shadowCube, vec4(dist, d)).r;
}

float pointShadowSmooth(samplerCubeShadow shadowCube, vec3 lightPos,
		float lightFarPlane, vec3 fragPos, float radius) {
	vec3 dist = fragPos - lightPos;
	float d = length(dist) / lightFarPlane;
	uint sampleCount = 20u;
	float accum = 0.f;
	for(uint i = 0; i < sampleCount; ++i) {
		vec3 dir = dist + radius * pointShadowOffsets[i];
		accum += texture(shadowCube, vec4(dir, d)).r;
	}

	return accum / sampleCount;
}
	
// manual comparison
float pointShadow(samplerCube shadowCube, vec3 lightPos, 
		float lightFarPlane, vec3 fragPos) {
	vec3 dist = fragPos - lightPos;
	float d = length(dist) / lightFarPlane;
	float closest = texture(shadowCube, dist).r * lightFarPlane;
	float current = length(dist);
	// const float bias = 0.01f;
	const float bias = 0.f;
	return current < (closest + bias) ? 1.f : 0.f;
}

vec3 multPos(mat4 transform, vec3 pos) {
	vec4 v = transform * vec4(pos, 1.0);
	return vec3(v) / v.w;
}

// normal encodings
// see http://jcgt.org/published/0003/02/01/paper.pdf
// using r16g16Snorm, this is oct32 from the paper
vec2 signNotZero(vec2 v) {
	return vec2((v.x >= 0.0) ? +1.0 : -1.0, (v.y >= 0.0) ? +1.0 : -1.0);
}

// n must be normalized
vec2 encodeNormal(vec3 n) {
	// spherical encoding (lattitude, longitude)
	// float latt = asin(n.y);
	// float long = asin(n.x / latt); // or acos(n.z / latt)
	// return vec2(latt, long);

	// naive
	// return vec2(n.x, n.y);
	
	// oct
	vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signNotZero(p)) : p;
}

// returns normalized vector
vec3 decodeNormal(vec2 n) {
	// spherical encoding (lattitude, longitude)
	// float xz = cos(n.x);
	// float y = sin(n.x);
	// return vec3(xz * sin(n.y), y, xz * cos(n.y));

	// naive
	// return vec3(n.x, n.y, sqrt(1 - n.x * n.x - n.y * n.y));
	
	// oct
	vec3 v = vec3(n.xy, 1.0 - abs(n.x) - abs(n.y));
	if (v.z < 0) v.xy = (1.0 - abs(v.yx)) * signNotZero(v.xy);
	return normalize(v);
}

// == screen space light scatter algorithms ==
vec3 sceneMap(mat4 proj, vec4 pos) {
	pos = proj * pos;
	vec3 mapped = pos.xyz / pos.w;
	mapped.y *= -1; // invert y
	mapped.xy = 0.5 + 0.5 * mapped.xy; // normalize for texture access
	return mapped;
}

vec3 sceneMap(mat4 proj, vec3 pos) {
	return sceneMap(proj, vec4(pos, 1.0));
}

// ripped from http://www.alexandre-pestana.com/volumetric-lights/
float mieScattering(float lightDotView, float gs) {
	float result = 1.0f - gs * gs;
	result /= (4.0f * 3.141 * pow(1.0f + gs * gs - (2.0f * gs) * lightDotView, 1.5f));
	return result;
}

// fragPos and lightPos are in screen space
// ldv: dot(lightDir, viewDir)
float lightScatterDepth(vec2 fragPos, vec2 lightPos, float lightDepth,
		float ldv, sampler2D depthTex, float fragDepth) {
	// if light position is outside of screen, we can't scatter since
	// we can't track rays to the ray (since depth map only covers screen)
	if(clamp(lightPos, 0.0, 1.0) != lightPos) {
		return 0.f;
	}

	vec3 ray = vec3(lightPos, lightDepth) - vec3(fragPos, fragDepth);
	// NOTE: making the number of steps dependent on the ray length
	// is probably a bad idea. When light an pixel (geometry) are close, the
	// chance for artefacts is the highest i guess
	// float l = dot(ray, ray); // max: 2
	// uint steps = uint(clamp(10 * l, 5, 15));

	uint steps = 10;
	vec3 step = ray / steps;
	float accum = 0.f;
	vec3 ipos = vec3(fragPos, fragDepth);

	// TODO: maybe more efficient ot pass pattern as texture?
	// or make global variable?
	// might be useful in multiple functions
	vec2 ppixel = mod(fragPos * textureSize(depthTex, 0), vec2(4, 4));
	const float ditherPattern[4][4] = {
		{ 0.0f, 0.5f, 0.125f, 0.625f},
		{ 0.75f, 0.22f, 0.875f, 0.375f},
		{ 0.1875f, 0.6875f, 0.0625f, 0.5625},
		{ 0.9375f, 0.4375f, 0.8125f, 0.3125}};
	float ditherValue = ditherPattern[int(ppixel.x)][int(ppixel.y)];
	ipos.xy += ditherValue * step.xy; // TODO: z too?

	// NOTE: instead of the dithering we could use a fully random
	// offset. Doesn't seem to work as well though.
	// Offset to step probably a bad idea
	// ipos += 0.7 * random(fragPos) * step;
	// step += 0.01 * (2 * random(step) - 1) * step;
	
	// sampling gets more important the closer we get to the light
	// important to not allow light to shine through completely
	// closed walls (because there e.g. is only one sample in it)
	float importance = 0.01;
	float total = 0.f;

	int lod = 0;
	// like ssao: we manually choose the lod since the default
	// derivative-based lod mechanisms aren't of much use here.
	// the larger the step size is, the less detail with need,
	// therefore larger step size -> high mipmap level
	// NOTE: nope, that doesn't work, destroys our random sampling
	// basically, that only works on higher levels...
	// vec2 stepPx = step * textureSize(depthTex, 0);
	// float stepLength = length(stepPx); // correct one, below are guesses
	// float stepLength = max(abs(stepPx.x), abs(stepPx.y));
	// float stepLength = min(abs(stepPx.x), abs(stepPx.y));
	// float stepLength = dot(stepPx, stepPx);
	// float lod = clamp(log2(stepLength) - 1, 0.0, 4.0);

	for(uint i = 0u; i < steps; ++i) {
		// sampler2DShadow: z value is the value we compare with
		// accum += texture(depthTex, vec3(ipos, rayEnd.z)).r;

		float depth = textureLod(depthTex, ipos.xy, lod).r;

		// TODO: don't make it binary for second condition
		// if(depth > lightDepth || depth < ipos.z) {
		if(depth > lightDepth) {
			accum += importance;
		}

		ipos += step;

		total += importance;
		importance *= 2;
	}

	// accum *= fac / steps;
	accum /= total;
	// accum = clamp(accum, 0.0, 1.0);
	// accum = smoothstep(0.1, 1.0, accum);

	// NOTE: random factors (especially the 35 seems weird...)
	// currently tuned for directional light
	float fac = 10 * mieScattering(ldv, 0.05);
	fac *= ldv;

	// nice small "sun" in addition to the all around scattering
	// fac += mieScattering(ldv, 0.95);

	// Make sure light gradually fades when light gets outside of screen
	// instead of suddenly jumping to 0 because of 'if' at beginning.
	// fac *= pow(lightPos.x * (1 - lightPos.x), 0.9);
	// fac *= pow(lightPos.y * (1 - lightPos.y), 0.9);
	fac *= 4 * lightPos.x * (1 - lightPos.x);
	fac *= 4 * lightPos.y * (1 - lightPos.y);

	return fac * accum;
}

/*
// TODO: fix. Needs some serious changes for point lights
float lightScatterShadow(vec3 pos) {
	// NOTE: first attempt at light scattering
	// http://www.alexandre-pestana.com/volumetric-lights/
	// problem with this is that it doesn't work if background
	// is clear.
	vec3 rayStart = scene.viewPos;
	vec3 rayEnd = pos;
	vec3 ray = rayEnd - rayStart;

	float rayLength = length(ray);
	vec3 rayDir = ray / rayLength;
	rayLength = min(rayLength, 8.f);
	ray = rayDir * rayLength;

	// float fac = 1 / dot(normalize(-ldir), rayDir);
	// TODO
	vec3 lrdir = light.pos.xyz - scene.viewPos;
	// float div = pow(dot(lrdir, lrdir), 0.6);
	// float div = length(lrdir);
	float div = 1.f;
	float fac = mieScattering(dot(normalize(lrdir), rayDir), 0.01) / div;

	const uint steps = 20u;
	vec3 step = ray / steps;
	// offset slightly for smoother but noisy results
	// TODO: instead use per-pixel dither pattern and smooth out
	// in pp pass. Needs additional scattering attachment though
	// rayStart += 0.1 * random(rayEnd) * rayDir;
	step += 0.01 * random(step) * step;
	rayStart += 0.01 * random(rayEnd) * step;

	float accum = 0.0;
	pos = rayStart;

	// TODO: falloff over time
	// float ffac = 1.f;
	// float falloff = 0.9;
	for(uint i = 0u; i < steps; ++i) {
		// position in light space
		// vec4 pls = light.proj * vec4(pos, 1.0);
		// pls.xyz /= pls.w;
		// pls.y *= -1; // invert y
		// pls.xy = 0.5 + 0.5 * pls.xy; // normalize for texture access
// 
		// // float fac = max(dot(normalize(light.pd - pos), rayDir), 0.0); // TODO: light.position
		// // accum += fac * texture(shadowTex, pls.xyz).r;
		// // TODO: implement/use mie scattering function
		// accum += texture(shadowTex, pls.xyz).r;

		accum += pointShadow(shadowTex, light.pos, pos);
		pos += step;
	}

	// 0.25: random factor, should be configurable
	// accum *= 0.25 * fac;
	accum *= fac;
	accum /= steps;
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}
*/

