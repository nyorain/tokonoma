#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 outPos; // xyz: pos, w: roughness
layout(location = 1) out vec4 outNormal; // xyz: normal, w: metallic
layout(location = 2) out vec4 outAlbedo; // rgb: albedo, w: occlusion

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(set = 1, binding = 1) uniform sampler2D metalRoughTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;

// factors
layout(push_constant) uniform materialPC {
	vec4 albedo;
	float roughness;
	float metallic;
	uint flags;
	float alphaCutoff;
} material;

// flags
const uint normalmap = (1u << 0);
const uint doubleSided = (1u << 1);


// NOTE: tangent and bitangent could also be passed in for each vertex
vec3 getNormal() {
	vec3 n = normalize(inNormal);
	if((material.flags & normalmap) == 0u) {
		return n;
	}

	// http://www.thetenthplanet.de/archives/1180
	vec3 q1 = dFdx(inPos);
	vec3 q2 = dFdy(inPos);
	vec2 st1 = dFdx(inUV);
	vec2 st2 = dFdy(inUV);

	vec3 t = normalize(q1 * st2.t - q2 * st1.t);
	vec3 b = -normalize(cross(n, t));

	// texture contains normal in tangent space
	// we could also just use a signed format here i guess
	vec3 tn = texture(normalTex, inUV).xyz * 2.0 - 1.0;
	return normalize(mat3(t, b, n) * tn);
}

void main() {
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	vec4 albedo = material.albedo * texture(albedoTex, inUV);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	vec3 normal = getNormal();
	if(!gl_FrontFacing) {
		normal *= -1;
	}

	outNormal.xyz = normal;
	outPos.xyz = inPos;
	outAlbedo.rgb = albedo.rgb;

	// as specified by gltf spec
	vec2 mr = texture(metalRoughTex, inUV).gb;
	outPos.w = material.roughness * mr.x;
	outNormal.w = material.metallic * mr.y;
	outAlbedo.w = texture(occlusionTex, inUV).r;
}
