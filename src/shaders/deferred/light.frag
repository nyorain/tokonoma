#version 450

layout(location = 0) out vec4 fragColor;

// scene
layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	vec3 viewPos;
} scene;

// gbuffer
layout(input_attachment_index = 0, set = 1, binding = 0)
	uniform subpassInput inPos;
layout(input_attachment_index = 0, set = 1, binding = 1)
	uniform subpassInput inNormal;
layout(input_attachment_index = 0, set = 1, binding = 2)
	uniform subpassInput inAlbedo;

layout(set = 2, binding = 0, row_major) uniform Light {
	mat4 matrix; // from global to light space
	vec3 pd; // position for point light, direction of dir light
	uint type; // point or dir
	vec3 color;
	uint pcf;
} light;

layout(set = 2, binding = 1) uniform sampler2DShadow shadowTex;

const uint pointLight = 1;
const uint dirLight = 2;


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
			// sampler has builtin comparison
			vec3 off = vec3(texelSize * vec2(x, y),  0);
			sum += texture(shadowTex, pos + off).r;
		}
	}

	float total = ((2 * range + 1) * (2 * range + 1));
	return sum / total;
}

void main() {
	vec4 sPos = subpassLoad(inPos);
	vec4 sNormal = subpassLoad(inNormal);
	vec4 sAlbedo = subpassLoad(inAlbedo);

	vec3 pos = sPos.xyz;
	vec3 normal = sNormal.xyz;
	vec3 albedo = sAlbedo.xyz;

	float roughness = sPos.w;
	float metallic = sNormal.w;
	float occlusion = sAlbedo.w;

	// TODO: remove random factors, implement pbr
	float ambientFac = 0.1 * occlusion;
	float diffuseFac = 0.5f;
	float specularFac = 0.5f;
	float shininess = 64.f;

	// position of texel in light space
	float lfac = 0.0;
	vec3 ldir = (light.type == pointLight) ?
		inPos - light.pd :
		light.pd;

	// diffuse
	lfac += diffuseFac * max(dot(normal, -ldir), 0.0);
	vec3 halfway = normalize(-ldir - vdir);
	lfac += specularFac * pow(max(dot(normal, halfway), 0.0), shininess);

	// blinn-phong specular
	vec3 vdir = normalize(inPos - scene.viewPos);

	// shadow
	vec4 lsPos = light.matrix * vec4(pos, 1.0);
	lfac *= shadowpcf(lsPos.xyz / lsPos.w, light.pcf);

	// ambient always added, indepdnent from shadow
	lfac += ambientFac;

	fragColor = vec4(lfac * light.color * albedo, 1.0);
}
