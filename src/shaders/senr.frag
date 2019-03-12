#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inPosm;

layout(location = 0) out vec4 outCol;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _proj;
	vec3 viewPos;
	uint showLightTex;
} scene;

layout(set = 1, binding = 0, row_major) uniform Model {
	mat4 _matrix;
	mat4 _normal;
	vec4 color;
} model;

layout(set = 1, binding = 1) uniform usamplerCube light;

// TODO: as ubo
const vec3 lightPos = vec3(0, 0, 0);

void main() {
	if(scene.showLightTex == 1) {
		vec3 light = unpackUnorm4x8(texture(light, inPosm).r).rgb;
		outCol = vec4(light, 1.0);
	} else {
		vec3 normal = normalize(inNormal);
		vec3 objectColor = model.color.rgb;
		float ambientFac = 0.1f;
		float diffuseFac = 0.4f;
		float specularFac = 0.4f;
		float shininess = 32.f;

		// ambient
		vec3 col = vec3(0.0);
		if(scene.showLightTex == 0) {
			vec3 col = ambientFac * objectColor;
		}

		// diffuse
		vec3 ldir = normalize(lightPos - inPos);
		col += diffuseFac * objectColor * max(dot(ldir, normal), 0.0);

		// blinn-phong
		vec3 vdir = normalize(scene.viewPos - inPos);
		vec3 h = normalize(vdir + ldir);
		col += specularFac * objectColor * pow(max(dot(normal, h), 0.0), shininess);

		if(scene.showLightTex == 2) {
			vec3 light = unpackUnorm4x8(texture(light, inPosm).r).rgb;
			col += light * objectColor;
		}

		outCol = vec4(col, 1.0);
	}

}