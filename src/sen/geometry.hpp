#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vecOps.hpp>

struct Ray {
	nytl::Vec3f origin;
	nytl::Vec3f dir;
};

// Box defined by a transform/3D affine coordinate system.
// The box that is defined will create the unit box (side length 2)
// when multiplied with the transform.
struct Box {
	Box() = default;

	/// Constructs the box with the given axes and center.
	/// The axes can be used to achieve rotation, scaling (a different
	/// box size) and even skewing. E.g. to get a box twice that big only
	/// in x direction / just pass {2, 0, 0} as new x axis.
	Box(nytl::Vec3f center,
			nytl::Vec3f x = {1.f, 0.f, 0.f},
			nytl::Vec3f y = {0.f, 1.f, 0.f},
			nytl::Vec3f z = {0.f, 0.f, 1.f}) {
		transform[0] = {x.x, x.y, x.z, -dot(x, center)};
		transform[1] = {y.x, y.y, y.z, -dot(y, center)};
		transform[2] = {z.x, z.y, z.z, -dot(z, center)};
		transform[3][3] = 1.f;
		auto xx = dot(x, x);
		auto yy = dot(y, y);
		auto zz = dot(z, z);
		if(xx > 0.f) { transform[0] *= 1 / xx; }
		if(yy > 0.f) { transform[1] *= 1 / yy; }
		if(zz > 0.f) { transform[2] *= 1 / zz; }
	}

	nytl::Mat4f transform = nytl::identity<4, float>();
};

using vec3 = nytl::Vec3f;
using namespace nytl::vec::cw::operators;
using namespace nytl::vec::cw;
using namespace nytl::vec::operators;

nytl::Vec3f multPos(const nytl::Mat4f& m, nytl::Vec3f v) {
	auto v4 = m * nytl::Vec4f{v.x, v.y, v.z, 1.f};
	return {v4[0] / v4[3], v4[1] / v4[3], v4[2] / v4[3]};
}

nytl::Vec3f multDir(const nytl::Mat4f& m, nytl::Vec3f v) {
	return static_cast<nytl::Mat3f>(m) * v;
}

float intersect(const Ray& r, const Box& b) {
	vec3 dir = multDir(b.transform, r.dir);
	vec3 o = multPos(b.transform, r.origin);

	// XXX:
	// could this be harmful in any case?
	// need it otherwise d,ad,t1,t2 can become NaN which might
	// propagate through end result (only in some cases though...)
	if(dir.x == 0.f) { dir.x += 0.00001; }
	if(dir.y == 0.f) { dir.y += 0.00001; }
	if(dir.z == 0.f) { dir.z += 0.00001; }

	// how many t we need to reach xi = 0 from origin
	// might result in +/- infinity in an dimension
	auto d = -o / dir;

	// t needed for ray to progress 1 in dimension i
	// might result in +/- infinity in an dimension
	auto ad = abs(1.f / dir);

	// ts needed for first/second xi = 1 intersections (following ray in its dir)
	// oi + di * t1i = sign(di)
	// oi + di * t2i = -sign(di)
	// again: might be infinity. Well defined though:
	auto t1 = d - ad;
	auto t2 = d + ad;

	using std::max, std::min;

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

    // vec3 normal = -sign(r.dir)* step(t1.yzx,t1.xyz)*step(t1.zxy,t1.xyz);

	// If the origin of the ray is *in* the box, then this
	// returns a negative value: the first intersection of the ray
	// behind the origin.
	return tn;
}
