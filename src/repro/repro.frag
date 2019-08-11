// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
// See src/repro/LICENSE

#version 450

#extension GL_GOOGLE_include_directive : enable
#include "scene.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0, row_major) uniform UBO {
	mat4 snapVP;
	mat4 invCamVP;
	vec3 camPos;
	float near;
	float far;
	float snapTime; // absolute; in seconds
	float time; // absolute; in seconds
	float exposure;
} ubo;

layout(set = 0, binding = 1) uniform sampler2D snapshotTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;

// const float zthresh = 0.01;
const float snapSpeed = 5; // in world units per second
const float snapLength = 20;

// TODO: we might get away with not rendering the scene again in every frame
// by simply tracing the ray for each pixel in the snapshotTex and see
// if depth matches (could use binary search even?). Might have even
// worse performance though and might not be as accurate i guess

float mrandom(vec4 seed4) {
	float dot_product = dot(seed4, vec4(12.9898,78.233,45.164,94.673));
	return fract(sin(dot_product) * 43758.5453);
}

void main() {
	float depth = texture(depthTex, uv).r;
	if(depth >= 1.0) {
		fragColor = vec4(0, 0, 0, 1);
		return;
	}

	// find reprojected texture coords
	vec3 world = reconstructWorldPos(uv, ubo.invCamVP, depth);
	vec3 snap = multPos(ubo.snapVP, world);
	snap.xy = 0.5 + 0.5 * vec2(snap.x, -snap.y);
	if(clamp(snap, 0, 1) != snap) {
		// nothing reprojected here; out of bounds
		fragColor = vec4(0, 0, 0, 1);
		return;
	}

	// snap.z = depthtoz(snap.z, ubo.near, ubo.far);

	// check whether the reprojected texture sample matches depth
	// pcf-like for anti aliasing
	// TODO: we can probably work with temporal super sampling + noise?
	/*
	float fac = 0.0;
	for(int x = -1; x <= 1; ++x) {
		for(int y = -1; y <= 1; ++y) {
			vec4 c = texture(snapshotTex, snap.xy + vec2(x, y) / textureSize(snapshotTex, 0));
			// float f = 1 - smoothstep(0.0, zthresh, abs(c.w - snap.z));
			// float f = abs(c.w - snap.z) < zthresh ? 1.f : 0.f;
			float f = snap.z < c.w + zthresh ? 1.f : 0.f;
			fac += f;
		}
	}

	fac /= 9;
	*/

	const vec2 poissonDisk[16] = vec2[]( 
	   vec2( -0.94201624, -0.39906216 ), 
	   vec2( 0.94558609, -0.76890725 ), 
	   vec2( -0.094184101, -0.92938870 ), 
	   vec2( 0.34495938, 0.29387760 ), 
	   vec2( -0.91588581, 0.45771432 ), 
	   vec2( -0.81544232, -0.87912464 ), 
	   vec2( -0.38277543, 0.27676845 ), 
	   vec2( 0.97484398, 0.75648379 ), 
	   vec2( 0.44323325, -0.97511554 ), 
	   vec2( 0.53742981, -0.47373420 ), 
	   vec2( -0.26496911, -0.41893023 ), 
	   vec2( 0.79197514, 0.19090188 ), 
	   vec2( -0.24188840, 0.99706507 ), 
	   vec2( -0.81409955, 0.91437590 ), 
	   vec2( 0.19984126, 0.78641367 ), 
	   vec2( 0.14383161, -0.14100790 ) 
	);

	float fac = 0.0;
	int count = 16;
	for(int i=0;i<count;i++){
		// we could make the length dependent on the
		// distance behind the first sample or something... (i.e.
		// make the shadow smoother when further away from
		// shadow caster).
		float len = 2 * mrandom(vec4(gl_FragCoord.xyy + 100 * world.xyz, i));
		float rid = mrandom(vec4(0.1 * gl_FragCoord.yxy - 32 * world.yzx, i));
		int id = int(16.0 * rid) % 16;
		vec2 off = len * poissonDisk[id] / textureSize(snapshotTex, 0).xy;
		// float z = texture(snapshotTex, snap.xy + off).w;
		// float z = depthtoz(texture(snapshotTex, snap.xy + off).r, ubo.near, ubo.far);
		float z = texture(snapshotTex, snap.xy + off).r;
		float f = snap.z < z ? 1.f : 0.f;
		fac += f / count;
	}

	// vec4 color = texture(snapshotTex, snap.xy);
	// float fac = 1 - smoothstep(0.0, zthresh, abs(color.w - snap.z));
	if(fac == 0.0) {
		// sample depth doesn't match, i.e. the sample that would
		// be reprojected here is occluded
		fragColor = vec4(0, 0, 0, 1);
		return;
	}

	// check if value from snapshot has already returned
	float dt = ubo.time - ubo.snapTime;
	float result = distance(ubo.camPos, world);

	if(dt * snapSpeed < result) {
		// the sample reprojected here is not yet available
		fragColor = vec4(0, 0, 0, 1);
		return;
	}

	fac *= 1 - smoothstep(result / snapSpeed, result / snapSpeed + snapLength, dt);

	// fragColor = vec4(color.rgb, 1.0);
	// tonemap depth for display
	snap.z = depthtoz(snap.z, ubo.near, ubo.far);
	fragColor = vec4(fac * vec3(exp(-ubo.exposure * snap.z)), 1.0);
}
