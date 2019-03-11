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
	float time;
} ubo;

layout(row_major, set = 0, binding = 1) readonly buffer Objects {
	Box boxes[];
};

// TODO: change dicard in main on count change
// layout(set = 0, binding = 2, rgba32f) uniform imageCube textures[6];
layout(set = 0, binding = 2, rgba8) uniform imageCube textures[6];

const vec3 up = vec3(0, 1, 0);
const vec3 lightPos = vec3(0, 0, 0);
const float INFINITY = 1.f / 0.f;
const float pi = 3.1415926535897932;

const int maxBounce = 4;

float random(float v) {
	return fract(sin(7.16294 * v) * 51812.142574);
}

vec2 random2(vec2 v) {
	return vec2(
		random(dot(v, vec2(15.32, 64.234))),
		random(dot(v, vec2(-35.2234, 24.23588453))));
}

vec3 random3(vec3 v) {
	return vec3(
		random(dot(v, vec3(63.45202, -14.034012, 53.35302))),
		random(dot(v, vec3(101.32, -23.990653, 29.53))),
		random(dot(v, vec3(-31.504, 83.23, 45.2))));
}

int sign21(float v) {
	return int(0.5 - 0.5 * sign(v));
}

// https://www.khronos.org/opengl/wiki/Template:Cubemap_layer_face_ordering
// https://www.khronos.org/registry/OpenGL/specs/gl/glspec42.core.pdf
// section 3.9.10
// TODO: fix border conditions
int cubeFace(vec3 dir, out vec2 suv) {
	vec3 ad = abs(dir);
	if(ad.x >= ad.y && ad.x >= ad.z) {
		if(dir.x > 0) {
			suv = vec2(-dir.z, -dir.y) / ad.x;
			return 0;
		} else {
			suv = vec2(dir.z, -dir.y) / ad.x;
			return 1;
		}
	} else if(ad.y > ad.x && ad.y >= ad.z) {
		if(dir.y > 0) {
			suv = vec2(dir.x, dir.z) / ad.y;
			return 2;
		} else {
			suv = vec2(dir.x, -dir.z) / ad.y;
			return 3;
		}
	} else { // z is largest
		if(dir.z > 0) {
			suv = vec2(dir.x, -dir.y) / ad.z;
			return 4;
		} else {
			suv = vec2(-dir.x, -dir.y) / ad.z;
			return 5;
		}
	}
}

bool anyhit(Ray ray, float belowt, uint ignore);

vec3 shade(vec3 pos, vec3 normal, vec3 view, uint ignore) {
	// sum of those should be 1?
	float diffFac = 0.4;
	float specFac = 0.4;

	float shininess = 32;

	vec3 col = vec3(0.0); // no ambient
	vec3 l = normalize(lightPos - pos);

	// check shadow
	Ray tolight = Ray(pos, lightPos - pos);

	// TODO: ignore original in anyhit
	// TODO: give light a size and use slightly random direction
	// here to light (inside light size) for smooth shadows
	if(dot(l, normal) > 0 && !anyhit(tolight, 1.f, ignore)) {
		// diffuse
		col += diffFac * clamp(dot(normal, l), 0, 1);

		// specular: blinn phong
		vec3 h = normalize(-view + l);
		col += specFac * vec3(clamp(pow(dot(h, normal), shininess), 0, 1));
	}

	return clamp(col, 0.0, 1.0);
}

// TODO: doesn't work like that with ignore in general...
bool anyhit(Ray ray, float belowt, uint ignore) {
	float t;
	// for(int i = 0; i < spheres.length(); ++i) {
	// 	t = intersect(ray, spheres[i].geom);
	// 	if(t > 0.0) {
	// 		return true;
	// 	}
	// }

	int l = boxes.length();
	for(uint i = 0; i < l; ++i) {
		if(i == ignore) {
			continue;
		}

		vec3 bnormal;
		t = intersect(ray, boxes[i], bnormal);
		// TODO: ignore original
		if(t > 0.0 && t < belowt) {
			return true;
		}
	}

	return false;
}

// next intersection t on ray.
// also gives normal and position of intersected object
// returns -1.0 if there is no intersection
float next(Ray ray, out vec3 pos, out vec3 normal,
		out vec3 uv, out uint id, uint ignore) {
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
		if(i == ignore) { // TODO: no self bounce atm
			continue;
		}

		vec3 bnormal;
		vec3 buv;
		t = intersect(ray, boxes[i], bnormal, buv);
		if(t > 0.0 && t < mint) {
			mint = t;
			pos = ray.origin + t * ray.dir;
			normal = bnormal;
			uv = buv;
			found = true;
			id = i;
		}
	}

	if(!found) {
		return -1.0;
	}

	return mint;
}

vec3 trace(Ray ray) {
	vec3 col = vec3(0, 0, 0); // default, background

	vec3 normal;
	vec3 pos;
	vec4 color;
	vec3 uv;
	uint id;
	uint ignore = 99999;

	uint ids[maxBounce];
	vec3 uvs[maxBounce];
	vec3 normals[maxBounce];
	vec3 dirs[maxBounce];
	float facs[maxBounce];
	vec3 poss[maxBounce];

	// forward till end
	int b;
	for(b = 0; b < maxBounce; ++b) {
		if(next(ray, pos, normal, uv, id, ignore) < 0.0) {
			break;
		}

		ids[b] = id;
		uvs[b] = uv;
		normals[b] = normal; // normalized
		dirs[b] = ray.dir;
		poss[b] = pos;

		// random bounce direction
		// TODO: better random direction choosing... start with more
		// relevant directions, make them generally more probable
		const vec3 anyv = normalize(vec3(-56.345, 12.1234, 1.23445)); // TODO
		float diff = dot(anyv, normal);
		vec3 c = anyv;
		if(abs(1.0 - diff) < 0.1) {
			c.xyz = c.yzx;
		}

		// if(normal == anyv) {
		// 	anyv.x = 0.0;
		// 	anyv.y = 1.0;
		// }
		vec3 v1 = normalize(cross(normal, c));
		vec3 v2 = normalize(cross(normal, v1));
		vec3 r = random3(pos + 0.1 * ubo.time * ray.dir);
		r.yz = 2 * r.yz - 1;
		// r.x *= 10; // more in normal dir?
		vec3 dir = vec3(0.0);
		dir += normalize(r.x * normal + r.y * v1 + r.z * v2);
		// dir += 10 * reflect(ray.dir, normal);
		// dir += 10 * normal;
		// dir = normalize(dir);
		
		// if(id == 5) { // reflective
		// 	dir = 10 * reflect(ray.dir, normal);
		// 	dir = normalize(dir);
		// }

		float f = dot(dir, normal);
		facs[b] = f;
		// facs[b] = 0.2;
		ray = Ray(pos, dir);
		ignore = id;
	}

	// shade
	int maxB = b - 1;
	for(--b; b >= 0; --b) {
		// TODO: shouldn't be applied to background color on first time?
		col *= facs[b];

		// TODO: bilinear interpolation on storing?
		// find face to store to in cube map
		uint id = ids[b];
		vec2 suvxy;
		int face = cubeFace(uvs[b], suvxy);
		suvxy = (0.5 + 0.5 * suvxy) * imageSize(textures[id]);
		ivec3 iuv = ivec3(ivec2(suvxy), face);

		// TODO: we can clamp to higher max when using float texture
		vec4 light = clamp(imageLoad(textures[id], iuv), 0, 1);
		// float fac = 0.05;
		float fac = (0.05 / maxBounce) * (maxBounce - b - 1); // more bounces -> more weight
		// if(id == 5) { // reflective
		// 	fac *= 5;
		// }
		// fac *= (1 - light.a); // make weaker over time? needs reset on scene change
		light = clamp(mix(light, vec4(col, 1.0), fac), 0, 1);
		imageStore(textures[ids[b]], iuv, light);

		// vec4 fcol = vec4(0.1);
		vec3 color = boxes[id].color.rgb;
		col = color * (light.rgb + shade(poss[b], normals[b], dirs[b], id));
		// col = 5 * light.rgb + color * shade(poss[b], normals[b], dirs[b], id);
		
		// XXX: object emission
		// works better with floating point textures
		// if(id == 5) {
			// col += vec3(0, 0, 500);
		// }
	
		// if(maxB > 0) {
		// 	col = 0.5 + 0.5 * dirs[1];
		// } else {
		// 	col = 0.1 + 0.1 * ray.dir;
		// 	// col = vec3(0.0);
		// }
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

	// TODO: change on count change
	if(boxes.length() != 6) {
		discard;
	}

	// random offset instead of antialiasing
	uv += (random2(ubo.time + uv) - 0.5) * pixel;

	// antialiasing steps
	// for no antialiasing se xc, yc to 0
	// count is number of rays sent per pixel
	int xc = 0;
	int yc = 0;
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
	// out_color = vec4(rgb, 1);

	// TODO: tone mapping
	float exposure = 1.0;
	float gamma = 1.0;
	vec3 mapped = vec3(1.0) - exp(-rgb * exposure);
	mapped = pow(mapped, vec3(1.0 / gamma));
	out_color = vec4(mapped, 1);
}
