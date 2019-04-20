#version 450

layout(location = 0) out vec4 fragColor;

// scene
layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	vec3 viewPos;
} scene;

// gbuffer
layout(set = 1, binding = 0, input_attachment_index = 0)
	uniform subpassInput inPos;
layout(set = 1, binding = 1, input_attachment_index = 1)
	uniform subpassInput inNormal;
layout(set = 1, binding = 2, input_attachment_index = 2)
	uniform subpassInput inAlbedo;

layout(set = 2, binding = 0, row_major) uniform Light {
	mat4 matrix; // from global to light space
	vec3 pd; // position for point light, direction of dir light
	uint type; // point or dir
	vec3 color;
	uint pcf;
} light;

layout(set = 2, binding = 1) uniform sampler2DShadow shadowTex;

layout(push_constant) uniform Show {
	uint mode;
} show;

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

	switch(show.mode) {
	case 1:
		fragColor = vec4(albedo, 1.0);
		return;
	case 2:
		fragColor = vec4(normal, 1.0);
		return;
	case 3:
		fragColor = vec4(pos, 1.0);
		return;
	case 4:
		fragColor = vec4(vec3(roughness), 1.0);
		return;
	case 5:
		fragColor = vec4(vec3(occlusion), 1.0);
		return;
	case 6:
		fragColor = vec4(vec3(metallic), 1.0);
		return;
	default:
		break;
	}

	// TODO: remove random factors, implement pbr
	float ambientFac = 0.1 * occlusion;
	float diffuseFac = 0.5f;
	float specularFac = 0.5f;
	float shininess = 64.f;

	// position of texel in light space
	float lfac = 0.0;
	vec3 ldir = (light.type == pointLight) ?
		pos - light.pd :
		light.pd;
	ldir = normalize(ldir);

	// diffuse
	lfac += diffuseFac * max(dot(normal, -ldir), 0.0);

	// blinn-phong specular
	vec3 vdir = normalize(pos - scene.viewPos);
	vec3 halfway = normalize(-ldir - vdir);
	lfac += specularFac * pow(max(dot(normal, halfway), 0.0), shininess);

	// shadow
	vec4 lsPos = light.matrix * vec4(pos, 1.0);
	lsPos.xyz /= lsPos.w;
	lsPos.y *= -1; // invert y
	lsPos.xy = 0.5 + 0.5 * lsPos.xy; // normalize for texture access
	lfac *= shadowpcf(lsPos.xyz, int(light.pcf));

	// ambient always added, indepdnent from shadow
	lfac += ambientFac;

	fragColor = vec4(lfac * light.color * albedo, 1.0);
}
