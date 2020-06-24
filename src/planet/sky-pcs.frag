#version 450

#extension GL_GOOGLE_include_directive : require

// Definitely supposed to mean preCosCat, btw, i.e. just the cat 
// instead of cos(cat).
#include "precoscat.hpp"
#include "color.glsl"
#include "scene.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform AtmosUBO {
	Atmosphere atmos;
	vec3 sunDir;
	float camAspect;
	vec3 viewPos;
	float camNear;
	vec3 camDir;
	float camFar;
	vec3 camUp;
	float camAspect;
};

layout(set = 0, binding = 1) uniform sampler2D transTex;
layout(set = 0, binding = 2) uniform sampler3D scatRayleighTex;
layout(set = 0, binding = 3) uniform sampler3D scatMieTex;
layout(set = 0, binding = 4) uniform sampler3D scatCombinedTex;

layout(set = 0, binding = 5) uniform sampler2D inColor;
layout(set = 0, binding = 6) uniform sampler2D inDepth;

layout(push_constant) uniform PCR {
	uint scatOrder;
};

vec3 getIn() {
	vec3 startPos = viewPos;
	vec3 viewDir = worldDirRay(vec2(inUV.x, 1 - inUV.y), 

	float r = length(startPos);
	float rmu = dot(startPos, viewDir);
	float distToTop = -rmu - sqrt(rmu * rmu - r * r + atmos.top * atmos.top);

	vec2 uv = inUVW.xy / inUVW.z;
	float depth = texture(inDepth, uv).r;
	float z = depthtoz(depth, near, far);
	vec3 bgColor = texture(inColor, uv).rgb;
	return bgColor; // TODO

	if(distToTop > 0.0 && (depth == 1.f || z > distToTop)) {
		startPos += distToTop * viewDir;
		r = atmos.top;
		rmu += distToTop;
	} else if(r > atmos.top) {
		return bgColor;
	}

	float mu = rmu / r;
	ARay ray = {r, mu};

	float mu_s = dot(startPos, sunDir) / r;
	float nu = dot(viewDir, sunDir);
	bool rayIntersectsGround = intersectsGround(atmos, ray);

	vec3 scat;
	// lookup from texture
	if(scatOrder == 1) {
		scat = getScattering(atmos, scatRayleighTex, scatMieTex,
			ray, mu_s, nu, rayIntersectsGround);
	} else {
		scat = getScattering(atmos, scatCombinedTex, scatMieTex,
			ray, mu_s, nu, rayIntersectsGround);
	}

	vec3 trans;
	if(depth == 1.f) {
		trans = transmittanceToTop(atmos, transTex, ray);
	} else {
		trans = getTransmittance(atmos, transTex, ray, depth, rayIntersectsGround);
	}

	scat += trans * bgColor;
	return scat;
}

void main() {
	vec3 scat = getIn();
	outColor = vec4(scat, 1.0);
}

