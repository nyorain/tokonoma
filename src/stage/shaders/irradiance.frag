#version 450

#extension GL_GOOGLE_include_directive : enable
#include "noise.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform UBO {
	vec3 x;
	float _1; // padding
	vec3 y;
	float _2; // padding
	vec3 z; // normal, cube map face
} dir;

layout(set = 0, binding = 1) uniform samplerCube envMap;

const float pi = 3.141; // good enough

void main() {
	vec2 uv = 2 * inUV.xy - 1; // normalize to [-1, 1]
	vec3 normal = normalize(dir.z + uv.x * dir.x + uv.y * dir.y);
	normal.y *= -1.f;

	vec3 accum = vec3(0.0);  
	vec3 up = vec3(0.0, 1.0, 0.0);
	vec3 right = cross(up, normal);
	up = cross(normal, right);

	float sampleDelta = 0.025;
	float nrSamples = 0.0; 
	for(float phi = 0.0; phi < 2.0 * pi; phi += sampleDelta) {
		for(float theta = 0.0; theta < 0.5 * pi; theta += sampleDelta) {
			// spherical to cartesian (in tangent space)
			vec3 tangentSample = vec3(sin(theta) * cos(phi),  sin(theta) * sin(phi), cos(theta));
			// tangent space to world
			vec3 sampleVec = tangentSample.x * right + 
				tangentSample.y * up +
				tangentSample.z * normal;

			accum += texture(envMap, sampleVec).rgb * cos(theta) * sin(theta);
			nrSamples++;
		}
	}
	accum = pi * accum * (1.0 / float(nrSamples));

	/*
	float total = 0.f;
	for(uint i = 0u; i < 100u; ++i) {
		vec3 tangentSample = random3(1 + i * normal);
		tangentSample.xy = 2 * pow(tangentSample.xy, vec2(2)) - 1;
		vec3 sampleVec = normalize(tangentSample.x * right + 
			tangentSample.y * up +
			tangentSample.z * normal);

		float fac = dot(sampleVec, normal);
		accum += fac * texture(envMap, sampleVec).rgb;
		total += fac;
	}
	accum /= total;
	*/

	outColor = vec4(accum, 1.0);
}
