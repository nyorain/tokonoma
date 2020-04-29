#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"
#include "pbr.glsl"

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inPosClip;
layout(location = 2) in vec4 inPosClipPrev;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec4 fragVel;

layout(set = 0, binding = 0, row_major) uniform Scene {
	mat4 _vp; // view and pojection
	mat4 _vpPrev;
	mat4 _vpInv;
	vec3 viewPos;
	float _;
	vec2 _jitter;
	float near;
	float far;
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
	vec3 light = vec3(0);
	if(dot(ldir, -n) >= 0) {
		float ld = length(ldir);

		light = cookTorrance(n, -ldir, -viewDir, roughness,
			metalness, vec3(1, 1, 1));
		light *= pointLight.color;
		light *= defaultAttenuation(ld, pointLight.radius);

		// pcf shadow
		float vd = length(scene.viewPos - inPos);
		float radius = (1.0 + (vd / 30.0)) / 100.0;
		light *= pointShadowSmooth(shadowCube, pointLight.pos,
			pointLight.radius, inPos, radius);
	}

	fragColor = vec4(light, 1.0);

	// velocity
	vec3 ndcCurr = inPosClip.xyz / inPosClip.w;
	vec3 ndcLast = inPosClipPrev.xyz / inPosClipPrev.w;
	ndcCurr.z = depthtoz(ndcCurr.z, scene.near, scene.far);
	ndcLast.z = depthtoz(ndcLast.z, scene.near, scene.far);
	vec3 vel = ndcCurr - ndcLast;
	fragVel = vec4(0.5 * vel.xy, vel.z, 0.0);
}

