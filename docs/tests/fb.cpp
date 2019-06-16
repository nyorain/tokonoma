#include <stage/fb.hpp>
#include <stage/types.hpp>
#include <nytl/approx.hpp>
#include <nytl/matOps.hpp>
#include <ostream>
#include "bugged.hpp"

using namespace doi;
using namespace nytl::approxOps;

TEST(bits) {
	auto fb0 = fb32(0);
	EXPECT(fb0.sign, 0u);
	EXPECT(fb0.bar, 0u);
	EXPECT(fb0.nums, 0u);

	// number representation the same as for i32 (up to 2^26)
	auto i = doi::i32(0);
	EXPECT(std::memcmp(&fb0, &i, 4), 0);

	/*
	// currently broken, see TODO's over class
	auto fb42 = fb32(42);
	EXPECT(fb42.sign, 0u);
	EXPECT(fb42.bar, 0u);
	EXPECT(fb42.nums, 42u);
	i = 42;
	EXPECT(std::memcmp(&fb42, &i, 4), 0);

	auto* hack = (const unsigned char*)(&fb42);
	std::cout << std::hex << (int)hack[0] << " " << (int)hack[1] << " " << (int)hack[2] << " " << (int)hack[3] << "\n";
	*/

	i = -123456;
	fb0 = fb32(-123456);
	EXPECT(std::memcmp(&fb0, &i, 4), 0);

	auto fb2 = fb32(2);
	EXPECT(fb2.sign, 0u);
	EXPECT(fb2.bar, 0u);
	EXPECT(fb2.nums, 2u);

	auto fbm13 = fb32(-13);
	EXPECT(fbm13.sign, 1u);
	EXPECT(fbm13.bar, 0u);
	EXPECT(fbm13.nums, 13u);
}

TEST(components) {
	auto c = components(fb32(-128));
	EXPECT(c.sign, 1u);
	EXPECT(c.nom, 128u);
	EXPECT(c.denom, 1u);

	c = components(fb32(0));
	EXPECT(c.sign, 0u);
	EXPECT(c.nom, 0u);
	EXPECT(c.denom, 1u);
}

TEST(div) {
	auto v = fb32div(-1, 2);
	EXPECT(v.sign, 1u);
	EXPECT(v.bar, 1u);
	EXPECT(v.nums, 0b10u); // [nom: 1]|[denom: (1)0]

	v = fb32div(-8, -7);
	EXPECT(v.sign, 0u);
	EXPECT(v.bar, 2u);
	EXPECT(v.nums, 0b100011u); // [nom: 1000]|[denom: (1)11]

	auto c = components(fb32div(1, 3));
	EXPECT(c.sign, 0u);
	EXPECT(c.nom, 1u);
	EXPECT(c.denom, 3u);

	c = components(fb32div(1, -3));
	EXPECT(c.sign, 1u);
	EXPECT(c.nom, 1u);
	EXPECT(c.denom, 3u);

	c = components(fb32div(-7, -2));
	EXPECT(c.sign, 0u);
	EXPECT(c.nom, 7u);
	EXPECT(c.denom, 2u);

	c = components(fb32div(-7, 92));
	EXPECT(c.sign, 1u);
	EXPECT(c.nom, 7u);
	EXPECT(c.denom, 92u);

	c = components(fb32div(129, -64));
	EXPECT(c.sign, 1u);
	EXPECT(c.nom, 129u);
	EXPECT(c.denom, 64u);
}

TEST(cmp) {
	EXPECT(int(fb32(1)), 1);
	EXPECT(int(fb32(2)), 2);
	EXPECT(int(fb32(3)), 3);

	EXPECT(int(fb32div(10, 5)), 2);
	EXPECT(int(fb32div(100, 1)), 100);
	EXPECT(fb32div(1000, 10), fb32(100));

	EXPECT(float(fb32pack(0, 1, 1)), 1.f);
	EXPECT(float(fb32div(1, -3)), -1.f / 3.f);
	EXPECT(float(fb32div(128, 64)), 2.f);
	EXPECT(int(fb32div(128, -64)), -2);

	EXPECT(float(fb32div(1, 12)), nytl::approx(1.f / 12.f));
}

TEST(ops) {
	auto third = fb32div(1, 3);
	EXPECT(third + third + third, fb32(1));
	EXPECT(fb32(2) + fb32(3), fb32(5));
	EXPECT(fb32div(8, 9) + fb32div(100, 200), fb32div(25, 18));

	EXPECT(int(fb32(9) * fb32div(1, 9)), 1);
	EXPECT(fb32div(-15590, 600) * fb32div(1, 12), fb32div(15590, -7200));

	EXPECT(fb32(2) - fb32(3), fb32(-1));
	EXPECT(-fb32(128) - fb32(-129), fb32(1));

	EXPECT(int(fb32(9) / fb32(3)), 3);
	EXPECT(fb32(1) / fb32div(234, 654), fb32div(654, 234));
	EXPECT(fb32div(-15590, 600) / fb32div(1, 12), fb32div(15590, -50));
}

TEST(large) {
	EXPECT(float(fb32(1e6) + fb32(2e6)), nytl::approx(3e6));
	EXPECT(fb32div(1e9, 2e9), fb32div(1, 2));
	EXPECT(fb32div(2e9, 1e9), fb32div(2, 1));

	// EXPECT(float(1e8 - 1) / float(1e8), nytl::approx(1.f));
	auto almost1 = fb32div(1e6 - 1, 1e6);
	EXPECT(float(almost1), nytl::approx(1.f, 0.01));
	auto two = fb32div(2e6, 1e6);
	EXPECT(float(two), nytl::approx(2.f, 0.01));
	EXPECT(float(almost1 + two), nytl::approx(3.f, 0.01));
}

// == less of a test, more of a playground ==
template<typename T>
nytl::Mat2<T> inverse2(const nytl::Mat2<T>& v) {
	return T(1) / (v[0][0] * v[1][1] - v[0][1] * v[1][0]) * nytl::Mat2<T>{
		v[1][1], -v[0][1],
		-v[1][0], v[0][0],
	};
}

TEST(inv_matrix) {
	using Mat2fb = nytl::Mat2<fb32>;
	auto m = Mat2fb{
		fb32div(4534, 57), fb32div(21, -10235),
		fb32div(-1323, 3), fb32div(2, 12552),
	};

	std::cout << "det: " << (m[0][0] * m[1][1] - m[0][1] * m[1][0]) << "\n";

	auto im = inverse2(m);
	std::cout << "inverse: " << im;
	std::cout << "(as float) inverse: " << nytl::Mat2f(im);
	std::cout << "inverse * m: " << (m * im);
	std::cout << "(as float) inverse * m: " << nytl::Mat2f(m * im);

	std::cout << " === float === \n";
	auto fm = nytl::Mat2f(m);
	auto ifm = inverse2(fm);
	std::cout << "inverse: " << ifm;
	std::cout << "inverse * m: " << (fm * ifm);
}

TEST(elimination_matrix) {
	auto m = nytl::Mat<3, 4, fb32> {
		fb32div(4534, 57), fb32div(21, -1235), fb32div(1, 5), fb32div(3, 1),
		fb32div(-1323, 3), fb32div(2, 1252), fb32div(4332, 12), fb32div(23, 54),
		fb32div(3, -5), fb32div(1, -3), fb32div(24, 412), fb32div(1, 3253),
	};
	auto fm = nytl::Mat<3, 4, float>(m);

	std::cout << " == rref == \n";
	nytl::reducedRowEcholon(m);
	std::cout << m << "\n";

	std::cout << " == rref float == \n";
	nytl::reducedRowEcholon(fm);
	std::cout << fm << "\n";
}
