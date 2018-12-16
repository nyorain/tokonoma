#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inLightPos;

layout(location = 0) out vec4 outCol;

const uint pointLight = 1;
const uint dirLight = 2;

struct Light {
	vec3 pd; // position for point light, direction of dir light
	uint type; // point or dir
	vec3 color;
	uint pcf; // padding
};

layout(constant_id = 0) const uint maxLightSize = 8;

layout(set = 0, binding = 0, row_major) uniform Lights {
	mat4 _proj;
	mat4 _light;
	Light lights[maxLightSize];
	vec3 viewPos; // camera position. For specular light
	uint numLights; // <= maxLightSize
} lights;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowTex;

// samples the shadow texture multiple times to get smoother shadow
// returns (1 - shadow), i.e. the light factor
float shadowpcf(vec3 pos) {
	if(pos.z > 1.0) {
		return 1.0;
	}

	vec2 texelSize = 1.f / textureSize(shadowTex, 0);
	float sum = 0.f;
	int range = int(lights.lights[0].pcf);
	for(int x = -range; x <= range; ++x) {
		for(int y = -range; y <= range; ++y) {
			vec3 off = vec3(texelSize * vec2(x, y),  0);
			sum += texture(shadowTex, inLightPos + off).r;
		}
	}

	float total = ((2 * range + 1) * (2 * range + 1));
	return sum / total;
}

void main() {
	vec3 normal = normalize(inNormal);
	vec3 objectColor = vec3(1, 1, 1);
	float ambientFac = 0.1f;
	float diffuseFac = 0.5f;
	float specularFac = 0.5f;
	float shininess = 64.f;

	// TODO: shadow coords only for first light supported

	vec3 col = vec3(0.1, 0.1, 0.1);
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
		lfac *= shadowpcf(inLightPos);

		// ambient, always added, even in shadow
		lfac += ambientFac;

		// combine
		col += lfac * light.color * objectColor;
	}

	outCol = vec4(col, 1.0);
}
