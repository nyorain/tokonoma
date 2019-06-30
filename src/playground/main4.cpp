#include <nytl/vecOps.hpp>
#include <dlg/dlg.hpp>

using namespace nytl;

int main() {
	// pbsa 5: 4.2
	// a
	// auto a = Vec3f{0, 0, 0};
	// auto b = Vec3f{1, 0, 0};
	// auto c = Vec3f{0, 1, 0};
	// auto d = Vec3f{0, 0, 1};

	// b
	auto a = Vec3f{0, 0, 0};
	auto b = Vec3f{0, 0, 1};
	auto c = Vec3f{0, -4, 0};
	auto d = Vec3f{1, 0, 0};

	auto v =  dot(a - d, cross(b - d, c - d)) / 6.f;
	dlg_info("volume: {}", v);

	auto f = 1 / (6.f * v);
	dlg_info(f * cross(b - d, c - d));
	dlg_info(f * cross(a - c, d - c));
	dlg_info(f * cross(a - d, b - d));
	dlg_info(f * cross(a - b, c - b));
}
