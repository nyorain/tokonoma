#include <nytl/matOps.hpp>
#include <dlg/dlg.hpp>

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

int main() {
	auto f = [](auto x) {
		return x * std::sin(x) - x;
	};

	auto x = newton(f);
	dlg_info(x);
	dlg_info(f(x));
}
