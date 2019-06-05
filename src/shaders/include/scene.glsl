#include "samples.glsl"
#include "noise.glsl"

// Light.flags
const uint lightDir = (1u << 0); // otherwise point
const uint lightPcf = (1u << 1); // use pcf
const uint lightShadow = (1u << 2); // use shadow

// MaterialPcr.flags
const uint normalMap = (1u << 0);
const uint doubleSided = (1u << 1);

const uint imageCount = 96u;
const uint samplerCount = 8u;

// TODO: don't hardcode. Instead pass per spec constant
const uint dirLightCascades = 4;

struct DirLight {
	vec3 color;
	uint flags;
	vec3 dir;
	float _; // unused padding
	mat4 cascadeProjs[dirLightCascades]; // global -> light space
	vec4 cascadeSplits[(dirLightCascades + 3) / 4];
};

struct PointLight {
	vec3 color;
	uint flags;
	vec3 pos;
	// float farPlane;
	float _;
	vec3 attenuation;
	float radius;
	mat4 proj[6]; // global -> cubemap [side i] light space
};

struct ModelData {
	mat4 matrix;
	mat4 normal; // [3]: {materialID, modelID, unused, unused}
};

struct MaterialTex {
	uint coords;
	uint id; // texture id
	uint samplerID;
};

struct Material {
	vec4 albedoFac;
	vec3 emissionFac;
	uint flags;
	float roughnessFac;
	float metallicFac;
	float alphaCutoff;
	MaterialTex albedo;
	MaterialTex normals;
	MaterialTex emission;
	MaterialTex metalRough;
	MaterialTex occlusion;
	vec2 pad;
};

// TODO: deprecated, remove!
// using Material in SSBOs instead
// size: 64
// struct MaterialPcr {
// 	vec4 albedo;
// 	vec3 emission;
// 	uint flags;
// 	float roughness;
// 	float metallic;
// 	float alphaCutoff;
// 	uint albedoCoords;
// 	uint emissionCoords;
// 	uint normalCoords;
// 	uint metalRoughCoords;
// 	uint occlusionCoords;
// };


// returns the z value belonging to the given depth buffer value
// obviously requires the near and far plane values that were used
// in the perspective projection.
// depth expected in standard vulkan range [0,1]
// NOTE: we could implement an alternative version that uses the projection
// matrix for depthtoz. But we always use the premultiplied
// projectionView matrix and we can't get the information out of that.
float depthtoz(float depth, float near, float far) {
	// return near * far / (far + near - depth * (far - near));
	return far * near / (far - depth * (far - near));
}
float ztodepth(float z, float near, float far) {
	// return (near + far - near * far / z) / (far - near);
	return (far - near * far / z) / (far - near);
}

// Reconstructs the fragment position in world space from the it's uv coord,
// the sampled depth ([0,1]) for this fragment and the scenes inverse viewProj.
// uv expected to have it's origin topleft, while world coord system has
// up y axis
vec3 reconstructWorldPos(vec2 uv, mat4 invViewProj, float depth) {
	vec2 suv = 2 * uv - 1;
	suv.y *= -1.f; // flip y, different directions in screen/world space
	vec4 pos4 = invViewProj * vec4(suv, depth, 1.0);
	return pos4.xyz / pos4.w;
}

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

// computes shadow for directional light
// returns (1 - shadow), i.e. the light factor
float dirShadow(sampler2DShadow shadowMap, vec3 pos, int range) {
	if(pos.z < 0.0 || pos.z > 1.0) {
		return 1.0;
	}

	vec2 texelSize = 1.f / textureSize(shadowMap, 0);
	float sum = 0.f;

	for(int x = -range; x <= range; ++x) {
		for(int y = -range; y <= range; ++y) {
			vec3 off = 1.5 * vec3(texelSize * vec2(x, y),  0);
			// sampler has builtin comparison
			sum += texture(shadowMap, pos + off).r;
		}
	}

	float total = ((2 * range + 1) * (2 * range + 1));
	return sum / total;
}

uint getCascadeIndex(DirLight light, float linearz) {
	uint i = 0u;
	for(; i < dirLightCascades; ++i) {
		if(linearz < light.cascadeSplits[i / 4][i % 4]) {
			break;
		}
	}
	return i;
}

uint getCascadeIndex(DirLight light, float linearz, out float between) {
	uint i = 0u;
	float last = 0.0;
	for(; i < dirLightCascades; ++i) {
		float split = light.cascadeSplits[i / 4][i % 4];
		if(linearz < split) {
			between = (linearz - last) / (split - last);
			return i;
		}
		last = split;
	}

	between = 0.f;
	return i;
}

float dirShadowIndex(DirLight light, sampler2DArrayShadow shadowMap,
		vec3 worldPos, uint index, int range) {
	// sampler has builtin comparison
	// array index comes *before* the comparison value (i.e. i before pos.z)
	vec3 pos = sceneMap(light.cascadeProjs[index], worldPos);
	if(pos.z <= 0.0 || pos.z >= 1.0) {
		return 0.0;
	}

	float sum = 0.f;

	// NOTE: not a good idea to use this here, apparently linear
	// filtering for shadow samplers doesn't really mean linear
	// filtering and instead just that they take multiple
	// pixels into account... can't realy on linear sampling
	// optimizations therefore
	/*
	// TODO: simpler with two seperate sampler objects probably
	const vec2 texSize = textureSize(shadowMap, 0).xy;
	const vec2 nf = 1.f / texSize; // normalization factor
	ivec2 ipos = ivec2(floor(pos.xy * texSize)); // unnormalized position
	vec4 access = vec4((ipos + 0.5) * nf, index, pos.z);
	// vec4 access = vec4(pos.xy, index, pos.z);
	sum += texture(shadowMap, access).r;

	access.xy = ipos * nf;
	sum += textureOffset(shadowMap, access, samplesLinear8Floor[0]).r;
	sum += textureOffset(shadowMap, access, samplesLinear8Floor[1]).r;
	sum += textureOffset(shadowMap, access, samplesLinear8Floor[2]).r;
	sum += textureOffset(shadowMap, access, samplesLinear8Floor[3]).r;
	sum += textureOffset(shadowMap, access, samplesLinear8Floor[4]).r;
	sum += textureOffset(shadowMap, access, samplesLinear8Floor[5]).r;
	sum += textureOffset(shadowMap, access, samplesLinear8Floor[6]).r;
	sum += textureOffset(shadowMap, access, samplesLinear8Floor[7]).r;
	// for(uint i = 0u; i < 4; ++i) {
	// 	vec2 off = nf * samplesLinear8[i];
	// 	sum += texture(shadowMap, vec4(pos.xy + off, index, pos.z));
	// 	sum += texture(shadowMap, vec4(pos.xy - off, index, pos.z));
	// }

	return sum / 9;
	*/

	const vec2 texelSize = 1.f / textureSize(shadowMap, 0).xy;
	for(int x = -range; x <= range; ++x) {
		for(int y = -range; y <= range; ++y) {
			vec2 off = texelSize * vec2(x, y);
			sum += texture(shadowMap, vec4(pos.xy + off, index, pos.z)).r;
		}
	}

	float total = ((2 * range + 1) * (2 * range + 1));
	return sum / total;
}

float dirShadow(DirLight light, sampler2DArrayShadow shadowMap, vec3 worldPos,
		float linearz, int range) {
	uint i = getCascadeIndex(light, linearz);
	return dirShadowIndex(light, shadowMap, worldPos, i, range);
}

// Calculates the light attentuation based on the distance d and the
// light attenuation parameters
// float attenuation(float d, vec3 params) {
float attenuation(float d, float radius) {
	// return 1.f / (params.x + params.y * d + params.z * (d * d));
	
	// TODO
	// hardcoded, normalized attenuation
	// at the outer border of the light (when d / radius = 1), the
	// light will have an attenuation of below 0.01, so at normal
	// exposure there will be no edge
	d /= radius;
	return 1.f / (1 + 8 * d + 96 * (d * d));
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
// using oct compression
vec2 encodeNormal(vec3 n) { 
	vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
	return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signNotZero(p)) : p;
}

// returns normalized vector
vec3 decodeNormal(vec2 n) {
	vec3 v = vec3(n.xy, 1.0 - abs(n.x) - abs(n.y));
	if (v.z < 0) v.xy = (1.0 - abs(v.yx)) * signNotZero(v.xy);
	return normalize(v);
}

// == screen space light scatter algorithms ==
// ripped from http://www.alexandre-pestana.com/volumetric-lights/
float mieScattering(float lightDotView, float gs) {
	float result = 1.0f - gs * gs;
	result /= (4.0f * 3.141 * pow(1.0f + gs * gs - (2.0f * gs) * lightDotView, 1.5f));
	return result;
}

// https://www.shadertoy.com/view/lslXDr
float phaseMie(float c, float g) {
	float cc = c * c;
	float gg = g * g;
	float a = ( 1.0 - gg ) * ( 1.0 + cc );
	float b = 1.0 + gg - 2.0 * g * c;
	b *= sqrt( b );
	b *= 2.0 + gg;	
	return (3.0 / 8.0 / 3.141) * a / b;
}

