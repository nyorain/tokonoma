#include <dlg/dlg.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/approxVec.hpp>
#include <algorithm>
using namespace nytl;

void g1() {
	Mat2f a {12, 7, 3, -8};
	auto mu = -8.f;
	auto bi = a - mu * identity<2, float>();
	auto lup = luDecomp(bi);
	auto z = Vec2f {1, 0};

	// auto inv = inverse(lup);
	// dlg_info("inv: {}", inv);
	// dlg_info("1: {}", normalized(inv * Vec2f {0, 1}));
	// dlg_info("1: {}", inv * Vec2f {0, 1});

	for(auto i = 1u; i < 10; ++i) {
		auto zd = luEvaluate(lup, z);
		dlg_assert(bi * zd == nytl::approx(z));
		auto r = dot(z, zd) / dot(z, z);
		z = Vec2f(normalized(zd));
		dlg_info("{}: z = {}, rayleigh = {}", i, z, r);
	}
}

template<size_t D, typename T>
auto maxNorm(const Vec<D, T>& v) {
	return *std::max_element(v.begin(), v.end());
}

void g3() {
	Mat2f a {0, 2, 1, -1};
	auto b = a;
	auto z = Vec2f {1, 2};

	// auto inv = inverse(lup);
	// dlg_info("inv: {}", inv);
	// dlg_info("1: {}", normalized(inv * Vec2f {0, 1}));
	// dlg_info("1: {}", inv * Vec2f {0, 1});

	for(auto i = 1u; i <= 10u; ++i) {
		auto r = dot(z, b * z) / dot(z, z);
		auto bz = b * z;
		z = (1 / maxNorm(bz)) * bz;
		dlg_info("{}: z = {}, rayleigh = {}", i, z, r);
	}
}

int main() {
	g3();
}
