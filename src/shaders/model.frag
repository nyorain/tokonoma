#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inLightPos; // position from pov light; for shadow
layout(location = 3) in vec2 inUV;

layout(location = 0) out vec4 outCol;

const uint pointLight = 1;
const uint dirLight = 2;

struct Light {
	vec3 pd; // position for point light, direction of dir light
	uint type; // point or dir
	vec3 color;
	uint pcf;
};

// TODO: remove this, we effictively only support one light in this
// forward renderer... at least regarding shadow
layout(constant_id = 0) const uint maxLightSize = 8;

layout(set = 0, binding = 0, row_major) uniform Lights {
	mat4 _proj;
	mat4 _light;
	Light lights[maxLightSize];
	vec3 viewPos; // camera position. For specular light
	uint numLights; // <= maxLightSize
} lights;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowTex;

// material
layout(set = 1, binding = 0) uniform sampler2D albedoTex;
layout(set = 1, binding = 1) uniform sampler2D metalRoughTex;
layout(set = 1, binding = 2) uniform sampler2D normalTex;
layout(set = 1, binding = 3) uniform sampler2D occlusionTex;

// flags
const uint normalmap = (1u << 0);
const uint doubleSided = (1u << 1);

// factors
layout(push_constant) uniform materialPC {
	vec4 albedo;
	float roughness;
	float metallic;
	uint flags;
	float alphaCutoff;
} material;

// samples the shadow texture multiple times to get smoother shadow
// pos: position in light space
// returns (1 - shadow), i.e. the light factor
float shadowpcf(vec3 pos, int range) {
	if(pos.z > 1.0) {
		return 1.0;
	}

	vec2 texelSize = 1.f / textureSize(shadowTex, 0);
	float sum = 0.f;
	for(int x = -range; x <= range; ++x) {
		for(int y = -range; y <= range; ++y) {
			vec3 off = vec3(texelSize * vec2(x, y),  0);
			sum += texture(shadowTex, pos + off).r;
		}
	}

	float total = ((2 * range + 1) * (2 * range + 1));
	return sum / total;
}

// NOTE: tangent and bitangent could also be passed in for each vertex
// then we could already compute light and view positiong in tangent space
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
	vec4 albedo = material.albedo * texture(albedoTex, inUV);
	if(albedo.a < material.alphaCutoff) {
		discard;
	}

	vec3 normal = getNormal();

	// NOTE: logic here has to be that weird (two independent if statements)
	// since otherwise i seem to trigger a bug (probably in driver or llvm?)
	// on mesas vulkan-radeon 19.0.2
	if(!gl_FrontFacing && (material.flags & doubleSided) == 0) {
		discard;
	}

	// otherwise flip normal, as gltf specifies it
	if(!gl_FrontFacing) {
		normal *= -1;
	}

	// TODO: actually use them...
	// vec2 mr = texture(metalRoughTex, inUV).rg;
	// float metalness = material.matallic * mr.b;
	// float roughness = material.roughness * mr.g;

	// TODO: remove random factors, implement pbr
	float ambientFac = 0.1 * texture(occlusionTex, inUV).r;
	float diffuseFac = 0.5f;
	float specularFac = 0.5f;
	float shininess = 64.f;

	// TODO: shadow coords only for first light supported
	// but that's one of the main problems of a forward renderer
	vec4 col = vec4(0.0);
	for(uint i = 0; i < lights.numLights; ++i) {
		Light light = lights.lights[i];
		vec3 ldir = (light.type == pointLight) ?
			inPos - light.pd :
			light.pd;
		ldir = normalize(ldir);

		float lfac = diffuseFac * max(dot(normal, -ldir), 0.0); // diffuse

		// specular
		vec3 vdir = normalize(inPos - lights.viewPos);

		// phong
		// vec3 refl = reflect(ldir, normal);
		// lfac += specularFac * pow(max(dot(-vdir, refl), 0.0), shininess);

		// blinn-phong
		vec3 halfway = normalize(-ldir - vdir);
		lfac += specularFac * pow(max(dot(normal, halfway), 0.0), shininess);

		// shadow
		lfac *= shadowpcf(inLightPos, int(light.pcf));

		// ambient, always added, even in shadow
		lfac += ambientFac;

		// combine
		col += vec4(lfac * light.color, 1.0) * albedo;
	}

	outCol = col;
	if(material.alphaCutoff == -1.f) { // alphaMode opque
		outCol.a = 1.f;
	}
}
