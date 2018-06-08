#include <dlg/dlg.hpp>
#include <nytl/matOps.hpp>
#include <nytl/vecOps.hpp>
using namespace nytl;

std::tuple<Vec2f, Vec2f> f1(Vec2f x, float t) {
	Mat2f a {-2, 1, 1, -2};
	auto front = a * x;
	auto back = Vec2f {};
	back.x += 2 * std::sin(t);
	back.y += 2 * (std::cos(t) - std::sin(t));
	return {front, back};
}

std::tuple<Vec2f, Vec2f> f2(Vec2f x, float t) {
	Mat2f a {-2, 1, 998, -999};
	auto front = a * x;
	auto back = Vec2f {};
	back.x += 2 * std::sin(t);
	back.y += 999 * (std::cos(t) - std::sin(t));
	return {front, back};
}

void ci() {
	auto x = Vec {2.f, 3.f};
	auto h = 0.001f;
	auto t = 0.f;
	for(auto i = 1u; i <= 500; ++i) {
		auto [front, back] = f2(x, t);
		x += h * (front + back);
		t += h;

		if(!(i % 100)) {
			dlg_info("{}: {}", i, x);
		}
	}
}

void cii() {
	auto x = Vec {2.f, 3.f};
	auto h = 0.1f;
	auto t = 0.f;

	Mat2f a {-2, 1, 998, -999};
	auto iha = identity<2, float>() - h * a;
	// Mat2f iha = {1.2, -0.1, -99.8, 100.9};
	auto inv = Mat2f(nytl::inverse(iha));
	dlg_info(iha);

	for(auto i = 1u; i <= 5; ++i) {
		auto back = Vec2f {};
		back.x += 2 * std::sin(t);
		back.y += 999 * (std::cos(t) - std::sin(t));
		x = inv * (x + h * back);
		t += h;

		dlg_info("{}: {}", i, x);
	}
}

int main() {
	cii();
}
