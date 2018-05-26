#include <dlg/dlg.hpp>
#include <nytl/matOps.hpp>

/*
template<std::size_t I, typename F, typename DF, typename V>
nytl::Vec<I, double> newton(F&& func, DF&& df, V start = {}) {
	constexpr auto eps = 1e-12;

	auto x = start;

	auto d = df(x);
	auto inv = nytl::inverse(d);
	x = x - inv * x;
}
*/

template<typename F, typename DF>
double newton(F&& f, DF&& df, double start = 0) {
	constexpr auto eps = 1e-16;
	constexpr auto maxSteps = 1024;
	auto x = start;

	for(auto i = 0u; i < maxSteps; ++i) {
		dlg_trace(x);
		auto d = df(x);
		auto step = (1. / d) * f(x);
		x = x - step;
		if(std::abs(step) < eps) {
			break;
		}
	}

	return x;
}

template<typename F>
double newton(F&& f, double start = 0) {
	return newton(f, [&](auto x) {
			constexpr auto d = 1e-10;
			return (f(x + d) - f(x - d)) / 2 * d;
		}, start);
}

/*
int main() {
	auto f = [](auto x) {
		return x * std::sin(x) - x;
	};

	auto x = newton(f);
	dlg_info(x);
	dlg_info(f(x));
}
*/

int main() {
	/*
	nytl::Mat3f m = {1, 0, 0, 2, 1, 0, 1, 2, 3};
	dlg_info(m * nytl::transpose(m));

	auto lup = nytl::LUDecomposition<3, float> {m, nytl::transpose(m), nytl::identity<3, bool>()};
	dlg_info(nytl::luEvaluate(lup, nytl::Vec3f {0, -1, 7}));
	dlg_info(((m * nytl::transpose(m)) * nytl::luEvaluate(lup, nytl::Vec3f {0, -1, 7})));
	*/

	nytl::Mat3f m = {2,6,4, 0,2,4, 1,4,6};
	auto lup = nytl::luDecomp(m);
	dlg_info("{} \n {} \n {}", lup.lower, lup.upper, lup.perm);
}
