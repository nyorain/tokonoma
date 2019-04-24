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
	mat4 proj[6]; // global -> cubemap [side i] light space
};

struct MaterialPcr {
	vec4 albedo;
	float roughness;
	float metallic;
	uint flags;
	float alphaCutoff;
};

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
// viewDir and lightDir must be normalized
float lightScatterDepth(vec2 fragPos, vec2 lightPos, float lightDepth,
		vec3 lightDir, vec3 viewDir, sampler2D depthTex) {
	// if light position is outside of screen, we can't scatter since
	// we can't track rays to the ray (since depth map only covers screen)
	if(clamp(lightPos, 0.0, 1.0) != lightPos) {
		return 0.f;
	}

	float ldv = -dot(lightDir, viewDir);
	float fac = mieScattering(ldv, 0.05);
	fac *= 35.0 * ldv;

	// nice small "sun" in addition to the all around scattering
	fac += mieScattering(ldv, 0.95);

	// Make sure light gradually fades when light gets outside of screen
	// instead of suddenly jumping to 0 because of 'if' at beginning.
	fac *= pow(lightPos.x * (1 - lightPos.x), 0.9);
	fac *= pow(lightPos.y * (1 - lightPos.y), 0.9);

	vec2 ray = lightPos - fragPos;

	// NOTE: making the number of steps dependent on the ray length
	// is probably a bad idea. When light an pixel (geometry) are close, the
	// chance for artefacts is the highest i guess
	// float l = dot(ray, ray); // max: 2
	// uint steps = uint(clamp(10 * l, 5, 15));

	uint steps = 10;
	vec2 step = ray / steps;
	float accum = 0.f;
	vec2 ipos = fragPos;

	vec2 ppixel = mod(fragPos * textureSize(depthTex, 0), vec2(4, 4));
	const float ditherPattern[4][4] = {
		{ 0.0f, 0.5f, 0.125f, 0.625f},
		{ 0.75f, 0.22f, 0.875f, 0.375f},
		{ 0.1875f, 0.6875f, 0.0625f, 0.5625},
		{ 0.9375f, 0.4375f, 0.8125f, 0.3125}};
	float ditherValue = ditherPattern[int(ppixel.x)][int(ppixel.y)];
	ipos += ditherValue * step;

	// NOTE: instead of the dithering we could use a fully random
	// offset. Doesn't seem to work as well though.
	// Offset to step probably a bad idea
	// ipos += 0.7 * random(fragPos) * step;
	// step += 0.01 * (2 * random(step) - 1) * step;
	for(uint i = 0u; i < steps; ++i) {
		// sampler2DShadow: z value is the value we compare with
		// accum += texture(depthTex, vec3(ipos, rayEnd.z)).r;

		float depth = texture(depthTex, ipos).r;
		accum += (depth < lightDepth ? 0.f : 1.f);
		ipos += step;
		if(ipos != clamp(ipos, 0, 1)) {
			break;
		}
	}

	accum *= fac / steps;
	accum = clamp(accum, 0.0, 1.0);
	return accum;
}
