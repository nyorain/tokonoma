#include <stage/f16.hpp>
#include <nytl/approx.hpp>
#include "bugged.hpp"

using doi::f16;
using namespace doi::f16_literal;
using namespace nytl::approxOps;
constexpr auto eps = 0.001;

TEST(conversion) {
	EXPECT(float(f16(1.f)), nytl::approx(1.f));
	EXPECT(float(f16(-1.f)), nytl::approx(-1.f));
	EXPECT(float(-f16(5.f)), nytl::approx(-5.f));

	auto fsmall = 0.001f;
	auto hsmall = f16(0.001f);
	EXPECT(hsmall.sign(), 0u);
	EXPECT(hsmall.exp() > 0 && hsmall.exp() < 15, true);
	EXPECT(hsmall.mantissa() != 0, true);

	EXPECT(float(hsmall), nytl::approx(fsmall, eps));
	EXPECT(float(f16(1200.f)), nytl::approx(1200.f));
}

TEST(special) {
	EXPECT(float(f16(0.f)), 0.f);
	EXPECT(float(f16(-0.f)), -0.f);
	EXPECT(float(f16(-0.f)), -0.f);

	auto finf = std::numeric_limits<float>::infinity();
	auto hinf = std::numeric_limits<f16>::infinity();
	EXPECT(hinf.sign(), 0u);
	EXPECT(hinf.exp(), 31u);
	EXPECT(hinf.mantissa(), 0u);

	EXPECT(float(hinf), finf);

	EXPECT(doi::isinf(hinf), true);
	EXPECT(doi::isinf(f16(0.f)), false);
	EXPECT(doi::isinf(-hinf), true);
	EXPECT(doi::isinf(f16(1.f)), false);

	EXPECT(doi::isnan(hinf), false);
	EXPECT(doi::isnan(f16(0.f)), false);
	EXPECT(doi::isnan(-hinf), false);
	EXPECT(doi::isnan(f16(1.f)), false);
}

TEST(ops) {
	EXPECT(1.0_f16 + 1.0_f16, nytl::approx(2.0_f16, eps));
	EXPECT(1.0_f16 - 1.0_f16, nytl::approx(0.0_f16, eps));
	EXPECT(3.0_f16 * 7.0_f16, nytl::approx(21.0_f16, eps));
	EXPECT(8.0_f16 * -(4.0_f16), nytl::approx(-32.0_f16, eps));
	EXPECT(9.0_f16 / -(3.0_f16), nytl::approx(-3.0_f16, eps));
	EXPECT(3.0_f16 / 1.5_f16, nytl::approx(2.0_f16, eps));
	EXPECT(2000.0_f16 / 3000.0_f16, nytl::approx(0.666666_f16, eps));
	EXPECT(9.0_f16 / 6.0_f16, nytl::approx(1.5, eps));
}

TEST(cmp) {
	EXPECT(1.0_f16, 1.0_f16);
	EXPECT(0.0_f16, -0.0_f16);
	EXPECT(3.0_f16 > 2.0_f16, true);
	EXPECT(3.0_f16 >= 2.0_f16, true);
	EXPECT(3.0_f16 != 2.0_f16, true);
	EXPECT(-3.0_f16 != 2.0_f16, true);
	EXPECT(-3.0_f16 <= 2.0_f16, true);
	EXPECT(-300.0_f16 < 2.0_f16, true);
	EXPECT(10.0_f16 >= 10.0_f16, true);
	EXPECT(10.0_f16 <= 10.0_f16, true);
	EXPECT(10.0_f16 == 10.0_f16, true);
	EXPECT(100000.0_f16 == 100000.0_f16, true);
	EXPECT(100000.0_f16 >= 100000.0_f16, true);
	EXPECT(100000.0_f16 <= 100000.0_f16, true);
	EXPECT(-100000.0_f16 <= 100000.0_f16, true);
	EXPECT(-100000.0_f16 < 100000.0_f16, true);
	EXPECT(-100000.0_f16 != 100000.0_f16, true);
	EXPECT(-f16(5.f), f16(-5.0));
}
