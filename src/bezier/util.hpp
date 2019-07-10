#include <tkn/types.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/math.hpp>
#include <cmath>

using namespace tkn::types;

// update nytl::mix?
template<typename P, typename T>
constexpr auto mix(const P& x, const P& y, T a) {
	return x + a * (y - x);
}

// bezier basis function
// degree n, at input t
constexpr float bezier(u32 n, float t) {
	using nytl::factorial;
	auto sum = 0.f;
	for(auto k = 0u; k < n; ++k) {
		auto f = factorial(n) * (factorial(k) * factorial(n - k));
		return f * std::pow(1 - t, n - k) * std::pow(t, k);
	}
	return sum;
}

template<u32 N>
struct Bezier {
	std::array<Vec3f, N + 1> points;
};

// iterative de casteljau algorithm
template<u32 N>
constexpr Vec3f eval(const Bezier<N>& bezier, float t) {
	// kind of arbitrary threshold but better than nothing
	static_assert(N < 4096, "This algorithm would use too much stack memory");
	static_assert(N > 0, "Bezier with N = 0 doesn't make sense");

	auto p = bezier.points;
	for(auto i = 0u; i < N; ++i) {
		for(auto j = 0u; j < N - i; ++j) {
			p[j] = mix(p[j], p[j + 1], t);
		}
	}

	return p[0];
}

// Will probably use more stack memory than the iterative version
// just for reference
template<u32 N>
constexpr Vec3f evalRec(const Bezier<N>& bezier, float t) {
	// kind of arbitrary threshold but better than nothing
	static_assert(N < 4096, "This algorithm would use too much stack memory");
	static_assert(N > 0, "Bezier with N = 0 doesn't make sense");

	Bezier<N - 1> next;
	for(auto i = 0u; i < N - 1; ++i) {
		next[i] = mix(bezier.points[i], bezier.points[i + 1], t);
	}

	return evalRec(next, t);
}

template<>
constexpr Vec3f evalRec<1>(const Bezier<1>& bezier, float) {
	return bezier.points[0];
}

// Bezier2, more efficient casteljau
struct Bezier2 {
	Vec3f start;
	Vec3f control;
	Vec3f end;
};

constexpr Vec3f eval(const Bezier2& b, float t) {
	Vec3f i1 = mix(b.start, b.control, t);
	Vec3f i2 = mix(b.control, b.end, t);
	return mix(i1, i2, t);
}

// dynamic
struct BezierN {
	std::vector<Vec3f> points;
};

Vec3f eval(const BezierN& bezier, float t) {
	auto p = bezier.points;
	for(auto i = 0u; i < p.size(); ++i) {
		for(auto j = 0u; j < p.size() - i; ++j) {
			p[j] = mix(p[j], p[j + 1], t);
		}
	}

	return p[0];
}

void subdivideR(std::vector<Vec3f>& ret, const Bezier<3>& bezier,
		unsigned lvl, unsigned maxlvl, float minSubdiv) {
	auto p1 = bezier.points[0];
	auto p2 = bezier.points[1];
	auto p3 = bezier.points[2];
	auto p4 = bezier.points[3];

	if(lvl > maxlvl) {
		ret.push_back(p4);
		return;
	}

	auto d = p4 - p1;
	auto c1 = cross(p2 - p4, d);
	auto c2 = cross(p3 - p4, d);
	// auto d2 = dot(c1, c1); // or length?
	// auto d3 = dot(c2, c2);
	auto d2 = length(c1);
	auto d3 = length(c2);

	if((d2 + d3) * (d2 + d3) <= minSubdiv * dot(d, d)) {
		ret.push_back(p4);
		return;
	}

	auto p12 = 0.5f * (p1 + p2);
	auto p23 = 0.5f * (p2 + p3);
	auto p34 = 0.5f * (p3 + p4);
	auto p123 = 0.5f * (p12 + p23);

	auto p234 = 0.5f * (p23 + p34);
	auto p1234 = 0.5f * (p123 + p234);

	subdivideR(ret, {p1, p12, p123, p1234}, lvl + 1, maxlvl, minSubdiv);
	subdivideR(ret, {p1234, p234, p34, p4}, lvl + 1, maxlvl, minSubdiv);
}

std::vector<Vec3f> subdivide(const Bezier<3>& bezier,
		unsigned maxLevel = 8, float minSubdiv = 0.0001f) {
	std::vector<Vec3f> ret;
	ret.push_back(bezier.points[0]);
	subdivideR(ret, bezier, 0, maxLevel, minSubdiv);
	return ret;
}
