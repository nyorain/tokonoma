#version 450

#extension GL_GOOGLE_include_directive : enable

#include "ray.glsl"

// TODO: structure data for more efficient traversal
// k-d tree or sth?
// see include/octree.glsl

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform UBO {
	vec4 pos;
	vec4 dir;
	float fov; // on y coord
	float aspect; // aspect ratio: x / y
	vec2 res; // resolution
} ubo;

layout(row_major, set = 0, binding = 1) readonly buffer Objects {
	Box boxes[];
};

const vec3 up = vec3(0, 1, 0);
const vec3 lightPos = vec3(0, 0, 0);
const float INFINITY = 1.f / 0.f;
const int maxBounce = 3;

// const Sphere spheres[] = {
// 	Sphere(vec4(0, 0, -3, 1), vec4(1, 1, 1, 1)),
// 	Sphere(vec4(0, -2, -5, 1), vec4(1, 0, 0, 1)),
// 	Sphere(vec4(3, 0, -4, 0.5), vec4(0, 1, 0, 1)),
// 	Sphere(vec4(1, 2, 3, 2), vec4(0, 0, 1, 1)),
// 	// {0, 5, 0, 0.1},
// };
//
// Box boxes[] = {
// 	box(vec3(-4.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0)),
// 	box(vec3(4.0, -4.0, 1.0), vec3(0.0, 1.0, 0.0)),
// 	box(vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0)),
// };

bool anyhit(Ray ray, float belowt);

vec3 shade(vec3 pos, vec3 normal, vec3 view) {
	// sum of those should be 1
	float ambientFac = 0.1;
	float diffFac = 0.4;
	float specFac = 0.4;

	float shininess = 32;

	vec3 col = vec3(0.1); // ambient
	vec3 l = normalize(lightPos - pos);

	// check shadow
	Ray tolight = Ray(pos, normalize(lightPos - pos));

	// TODO: ignore original in anyhit
	if(!anyhit(tolight, 1.f)) {
		// diffuse
		col += diffFac * clamp(dot(normal, l), 0, 1);

		// specular: blinn phong
		vec3 h = normalize(-view + l);
		col += specFac * vec3(clamp(pow(dot(h, normal), shininess), 0, 1));
	}

	return clamp(col, 0.0, 1.0);
}

bool anyhit(Ray ray, float belowt) {
	float t;
	// for(int i = 0; i < spheres.length(); ++i) {
	// 	t = intersect(ray, spheres[i].geom);
	// 	if(t > 0.0) {
	// 		return true;
	// 	}
	// }

	int l = boxes.length();
	for(uint i = 0; i < l; ++i) {
		vec3 bnormal;
		t = intersect(ray, boxes[i], bnormal);
		// TODO: ignore original
		if(t > 0.001 && t < belowt) {
			return true;
		}
	}

	return false;
}

// next intersection t on ray.
// also gives normal and position of intersected object
// returns -1.0 if there is no intersection
float next(Ray ray, out vec3 pos, out vec3 normal, out vec4 color) {
	float t;
	float mint = 1.f / 0.f; // infinity
	bool found = false;
	// for(int i = 0; i < spheres.length(); ++i) {
	// 	t = intersect(ray, spheres[i].geom);
	// 	if(t > 0.0 && t < mint) {
	// 		mint = t;
	// 		pos = ray.origin + t * ray.dir;
	// 		normal = sphereNormal(spheres[i].geom.xyz, pos);
	// 		color = spheres[i].color;
	// 		found = true;
	// 	}
	// }

	int l = boxes.length();
	for(uint i = 0; i < l; ++i) {
		vec3 bnormal;
		t = intersect(ray, boxes[i], bnormal);
		if(t > 0.0 && t < mint) {
			mint = t;
			pos = ray.origin + t * ray.dir;
			normal = bnormal;
			color = boxes[i].color;
			found = true;
		}
	}

	if(!found) {
		return -1.0;
	}

	return mint;
}

vec3 trace(Ray ray) {
	vec3 col = vec3(0, 0, 0); // default, background
	float fac = 1.0;

	vec3 normal;
	vec3 pos;
	vec4 color;

	for(int b = 0; b < maxBounce; ++b) {
		if(next(ray, pos, normal, color) < 0.0) {
			break;
		}

		col += fac * vec3(color) * shade(pos, normal, ray.dir);
		fac *= 0.2;
		ray = Ray(pos, reflect(ray.dir, normal));
	}

	return col;
}

void main() {
	vec2 pixel = 1 / ubo.res;
	vec3 az = normalize(ubo.dir.xyz);
	vec3 ax = normalize(cross(az, up));
	vec3 ay = cross(ax, az);

	vec2 uv = 2 * in_uv - 1;
	uv.y = -uv.y;
	float maxy = tan(ubo.fov / 2);

	// antialiasing steps
	// for no antialiasing se xc, yc to 0
	// count is number of rays sent per pixel
	int xc = 2;
	int yc = 2;
	float count = (1 + 2 * xc) * (1 + 2 * yc);
	vec3 rgb = vec3(0, 0, 0);
	for(int x = -xc; x <= xc; ++x) {
		for(int y = -yc; y <= yc; ++y) {
			vec2 muv = uv;
			muv += vec2(x, y) * pixel;
			muv.y *= maxy;
			muv.x *= maxy * ubo.aspect;
			vec3 ro = ubo.pos.xyz;
			vec3 rd = normalize(az + muv.x * ax + muv.y * ay);

			vec3 col = trace(Ray(ro, rd));
			rgb += (1 / count) * col;
		}
	}

	// tone mapping
	out_color = vec4(rgb, 1);

	// TODO: tone mapping
	// float exposure = 1.0;
	// float gamma = 1.0;
	// vec3 mapped = vec3(1.0) - exp(-rgb * exposure);
	// mapped = pow(mapped, vec3(1.0 / gamma));
	// out_color = vec4(mapped, 1);
}
