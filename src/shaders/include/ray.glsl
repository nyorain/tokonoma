#define EXACT

struct Ray {
	vec3 origin;
	vec3 dir;
};

// box defined by a transform
// transform * box = unit cube
struct Box {
	mat4 transform;
	// mat4 normal;
	vec4 color;
};

struct Sphere {
	vec4 geom;
	vec4 color;
};

vec3 pos(Sphere s) {
	return s.geom.xyz;
}

float radius(Sphere s) {
	return s.geom.w;
}

/*
Box box() {
	return Box(mat4(1), vec4(1.0), mat4(1));
}

Box box(vec3 center, vec3 color) {
	Box b = box();
	b.transform[3].xyz = center;
	b.color = vec4(color, 1.0);
	return b;
}

Box box(vec3 center, vec3 x, vec3 y, vec3 z) {
	return Box(
		mat4(vec4(x, 0), vec4(y, 0), vec4(z, 0), vec4(center, 1)),
		vec4(1.0));
}
*/

vec3 multDir(mat4 transform, vec3 dir) {
	return mat3(transform) * dir;
}

vec3 multPos(mat4 transform, vec3 pos) {
	vec4 v = transform * vec4(pos, 1.0);
	return vec3(v) / v.w;
}

/// Intersects the ray with the sphere at pos sphere.xyz with radius sphere.w
float intersect(Ray r, vec4 sphere) {
    vec3 oc = r.origin - sphere.xyz;
    float b = dot(oc, r.dir);
    float c = dot(oc, oc) - sphere.w * sphere.w;
    float h = b*b - c;
    if(h < 0.0) { // no intersection
		return -1.0;
	}
	return -b - sqrt(h);
}

vec3 sphereNormal(vec3 ce, vec3 pos) {
	return normalize(pos - ce);
}

/// Returns intersection ray parameter and box normal of ray and box.
/// -1.0 when there is no intersection, a negative value if the
/// ray origin is inside the box.
float intersect(Ray r, Box b, out vec3 normal) {
	vec3 dir = multDir(b.transform, r.dir);
	vec3 o = multPos(b.transform, r.origin);

#ifdef EXACT
	// XXX:
	// could this be harmful in any case?
	// need it otherwise d,ad,t1,t2 can become NaN which might
	// propagate through end result (only in some cases though...)
	if(dir.x == 0.f) { dir.x += 0.00001; }
	if(dir.y == 0.f) { dir.y += 0.00001; }
	if(dir.z == 0.f) { dir.z += 0.00001; }
#endif

	// how many t we need to reach xi = 0 from origin
	// might result in +/- infinity in an dimension
	vec3 d = -o / dir;

	// t needed for ray to progress 1 in dimension i
	// might result in +/- infinity in an dimension
	vec3 ad = abs(1.f / dir);

	// ts needed for first/second xi = 1 intersections (following ray in its dir)
	// oi + di * t1i = sign(di)
	// oi + di * t2i = -sign(di)
	// again: might be infinity. Well defined though:
	vec3 t1 = d - ad;
	vec3 t2 = d + ad;

	// the t value on which the ray has entered (i.e. crossed the first
	// intersection point; must be from the outside) the unit cube in
	// all dimensions.
	// That's why we need max here: the last one.
	float tn = max(max(t1.x, t1.y), t1.z);

	// the t value on which the ray has left (i.e. crossed the second
	// intersection point; must be from the inside) the unit cube
	// in the first dimensions.
	// That's why we need min here: the first one.
	float tf = min(min(t2.x, t2.y), t2.z);

	// if the ray leaves the unit cube in one dimension while not
	// having entered in all, it will never intersect.
	// When otherwise tf < 0.0, then the first intersection, then
	// both intersections are on negative t values.
	if(tn > tf || tf < 0.0) {
		return -1.f;
	}

    normal = -sign(r.dir) * step(t1.yzx, t1.xyz) * step(t1.zxy, t1.xyz);

	// If the origin of the ray is *in* the box, then this
	// returns a negative value: the first intersection of the ray
	// behind the origin.
	return tn;
}

// also returns the position on the unit cube in bpos
float intersect(Ray r, Box b, out vec3 normal, out vec3 bpos) {
	vec3 dir = multDir(b.transform, r.dir);
	vec3 o = multPos(b.transform, r.origin);

#ifdef EXACT
	if(dir.x == 0.f) { dir.x += 0.00001; }
	if(dir.y == 0.f) { dir.y += 0.00001; }
	if(dir.z == 0.f) { dir.z += 0.00001; }
#endif

	vec3 d = -o / dir;
	vec3 ad = abs(1.f / dir);

	vec3 t1 = d - ad;
	vec3 t2 = d + ad;

	float tn = max(max(t1.x, t1.y), t1.z);
	float tf = min(min(t2.x, t2.y), t2.z);

	if(tn > tf || tf < 0.0) {
		return -1.f;
	}

	// TODO: doesn't work that well with many transform matrices...
	// fix that
    normal = -sign(r.dir) * step(t1.yzx, t1.xyz) * step(t1.zxy, t1.xyz);
	// normal = normalize(multDir(b.normal, normal));
	bpos = o + tn * dir;

	return tn;
}