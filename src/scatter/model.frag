#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "pbr.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _vp; // view and pojection
	vec3 viewPos;
} scene;

layout(set = 1, binding = 0, row_major) uniform PointLightBuf {
	PointLight pointLight;
};

layout(set = 1, binding = 1) uniform samplerCubeShadow shadowCube;

void main() {
	float roughness = 1.0;
	float metalness = 0.0;

	vec3 dx = dFdx(inPos - scene.viewPos);
	vec3 dy = dFdy(inPos - scene.viewPos);
	vec3 n = normalize(-cross(dx, dy));

	vec3 viewDir = normalize(inPos - scene.viewPos);
	vec3 ldir = inPos - pointLight.pos;
	float ld = length(ldir);

	vec3 light = cookTorrance(n, -ldir, -viewDir, roughness,
		metalness, vec3(1, 1, 1));
	light *= pointLight.color;
	light *= defaultAttenuation(ld, pointLight.radius);

	// pcf shadow
	float vd = length(scene.viewPos - inPos);
	float radius = (1.0 + (vd / 30.0)) / 100.0;
	light *= pointShadowSmooth(shadowCube, pointLight.pos,
		pointLight.radius, inPos, radius);

	fragColor = vec4(light, 1.0);
}

