#version 450

// we treat all coordinates as in range [-1, 1]
// with y axis going upwards
// z axis is (as in opengl/vulkan convention) coming out of screen

#extension GL_GOOGLE_include_directive : require
#include "snoise.glsl"
#include "noise.glsl"

layout(location = 0) in vec2 inuv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;
	uint effect; // ignored

	vec3 camPos;
	float aspect;
	vec3 camDir;
	float fov;
} ubo;

struct Ray {
	vec3 origin;
	vec3 dir;
};

vec3 at(Ray ray, float t) {
	return ray.origin + t * ray.dir;
}

vec3 mapCube(vec3 p, vec3 center, float halflength) {
	p = (p - center) / halflength;
	return 0.05 * float(p == clamp(p, -1.f, 1.f)) * vec3(0.5 * p + 1);
}

vec3 mapSphere(vec3 p, vec3 center, float radius) {
	// sphere with radius 0 at position (0, 0, -4)
	p -= center;
	return 0.01 * float(length(p) < radius) * vec3(1);
}

float fbm(vec3 p) {
	float f = snoise(p) +
		0.5 * snoise(2 * p) +
		0.25 * snoise(4 * p) +
		0.125 * snoise(8 * p) +
		0.06125 * snoise(16 * p);
	return f;
}

float fbmv(vec3 p) {
	float f = voronoiNoise(p) +
		0.5 * voronoiNoise(2 * p) +
		0.25 * voronoiNoise(4 * p);
	return f;
}

// TODO: no idea what this is, really
// just the snoise gradient i guess. That's not too interesting,
// could be computed *way* more efficiently
vec3 noCurlNoise(vec3 v) {
	float eps = 0.001;
	float eps2 = 2 * eps;

	float x1 = snoise(v + vec3(eps, 0, 0));
	float x2 = snoise(v + vec3(-eps, 0, 0));
	float dx = (x2 - x1) / eps2;

	float y1 = snoise(v + vec3(0, eps, 0));
	float y2 = snoise(v + vec3(0, -eps, 0));
	float dy = (y2 - y1) / eps2;

	float z1 = snoise(v + vec3(0, 0, eps));
	float z2 = snoise(v + vec3(0, 0, -eps));
	float dz = (z2 - z1) / eps2;

	return vec3(dx, dy, dz);
}

vec3 snoiseVec3( vec3 x ){
  float s  = snoise(vec3( x ));
  float s1 = snoise(vec3( x.y - 19.1 , x.z + 33.4 , x.x + 47.2 ));
  float s2 = snoise(vec3( x.z + 74.2 , x.x - 124.5 , x.y + 99.4 ));
  vec3 c = vec3( s , s1 , s2 );
  return c;
}

vec3 curlNoise( vec3 p ){
  const float e = .1;
  vec3 dx = vec3( e   , 0.0 , 0.0 );
  vec3 dy = vec3( 0.0 , e   , 0.0 );
  vec3 dz = vec3( 0.0 , 0.0 , e   );

  vec3 p_x0 = snoiseVec3( p - dx );
  vec3 p_x1 = snoiseVec3( p + dx );
  vec3 p_y0 = snoiseVec3( p - dy );
  vec3 p_y1 = snoiseVec3( p + dy );
  vec3 p_z0 = snoiseVec3( p - dz );
  vec3 p_z1 = snoiseVec3( p + dz );

  float x = p_y1.z - p_y0.z - p_z1.y + p_z0.y;
  float y = p_z1.x - p_z0.x - p_x1.z + p_x0.z;
  float z = p_x1.y - p_x0.y - p_y1.x + p_y0.x;

  const float divisor = 1.0 / ( 2.0 * e );
  return normalize( vec3( x , y , z ) * divisor );

}

float cloudLow = 0.f;
float cloudHigh = 1.f;
vec3 mapClouds(vec3 p) {
	if(p.y < cloudLow || p.y > cloudHigh) {
		return vec3(0.0);
	}

	vec3 dir = vec3(1, 0.0, -0.34);
	// p += 1.0 * curlNoise(p);
	p += 0.5 * ubo.time * dir;

	p.xz *= 0.2;
	p.y *= 2;
	float offg = fbm(0.05 * ubo.time + 0.25 * p);

	// float off = -0.8;
	float off = -float(ubo.effect - 40.f) / 40.f;
	off += offg;

	float v = fbm(p) + off + 1;
	float fac = 5.0;
	return vec3(clamp(fac * v, 0, 1000));
}

vec3 mapClouds2(vec3 p) {
	if(p.y < 0.f || p.y > 1.f) {
		return vec3(0.0);
	}

	// float off = -0.8;
	float off = -float(ubo.effect - 20.f) / 40.f;
	off -= (1 - smoothstep(0.0, 0.25, p.y)) +
		smoothstep(0.75, 1.0, p.y);

	p *= 4;
	float v = clamp(fbmv(p) + off, 0, 1);
	float fac = 1.0;
	// float fac = 30.0 * smoothstep(0.0, 0.25, p.y)
		// (1 - smoothstep(0.99, 1.0, p.y));
	return vec3(fac * v);
}


vec3 scene(vec3 p) {
	// return mapCube(p, vec3(0, 0, -4), 0.5) +
	// 	mapSphere(p, vec3(2, 3, -12), 2);

	return mapClouds(p);
	// return mapClouds2(p);
	// return mapClouds(p) + 0.1 * mapClouds2(p);

	// return 0.01 * curlNoise(p);
}

// Henyey-Greenstein
float phase(float cosTheta, float g) {
	float fac = .079577471545; // 1 / (4 * pi), normalization
	float gg = g * g;
	return fac * (1 - gg) / (pow(1 + gg - 2 * g * cosTheta, 1.5));
}

const vec3 sunDir = normalize(vec3(1, -1, 0.2));
const vec3 sunColor = 3 * vec3(1.0, 0.9, 0.7);

void main() {
	vec2 uv = 2 * inuv - 1;
	uv.y *= -1;

	const vec3 up = vec3(0, 1, 0);
	vec3 dir = normalize(ubo.camDir);
	vec3 x = normalize(cross(dir, up));
	vec3 y = cross(x, dir);

	float maxy = tan(ubo.fov / 2);
	uv *= vec2(maxy * ubo.aspect, maxy);

	Ray ray;
	ray.origin = ubo.camPos;
	ray.dir = normalize(dir + uv.x * x + uv.y * y);

	if(ray.dir.y > 0.0 && ray.origin.y < 0.0) {
		float t = (0.0 - ray.origin.y) / ray.dir.y;
		ray.origin += t * ray.dir;
	} else if(ray.dir.y < 0.0 && ray.origin.y > cloudHigh) {
		float t = (cloudHigh - ray.origin.y) / ray.dir.y;
		ray.origin += t * ray.dir;
	}

	vec3 diff = ray.origin - ubo.camPos;
	if(dot(diff, diff) > 1000.0) {
		outcol = vec4(vec3(0.0), 1.f);
		return;
	}

	vec3 col = vec3(0, 0, 0);
	float t = 0.f;

	// TODO(idea): use higher order integral approximation?
	// raymarch over map
	float step = 0.1;
	t += step * random(gl_FragCoord.xy + ubo.time);

	float transmittance = 1.0;
	float inScatter = 0.0;
	for(int i = 0; i < 50; ++i) {
		vec3 pos = at(ray, t);
		if((pos.y < cloudLow && ray.dir.y < 0.f) ||
				(pos.y > cloudHigh && ray.dir.y > 0.f)) {
			break;
		}

		float density = scene(pos).r;
		float transmit = exp(-3 * step * density);

		transmittance *= transmit;
		inScatter += density * step * transmittance;

		t += step;

		// if(transmittance < 0.1) {
		// 	transmittance = 0.0;
		// 	break;
		// }
	}

	const float g = 0.5f;
	float cosTheta = dot(-ray.dir, sunDir);
	vec3 light = inScatter * phase(cosTheta, g) * sunColor;

	vec3 bg = vec3(0.6, 0.6, 0.9);
	// vec3 color = transmittance * bg + light;
	vec3 color = light;

	// tonemap
	float exposure = 1.0;
	color = 1.0 - exp(-exposure * color);

	// col = pow(col, vec3(1.0 / 2.2));
	outcol = vec4(color, 1.f);
}
