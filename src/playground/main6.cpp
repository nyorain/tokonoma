// gdv 2, exercise 6.3.d

#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <dlg/dlg.hpp>

using namespace nytl;

float dist(Vec2f point, Vec2f ep, Vec2f n) {
	return dot(n, point - ep);
}

int main() {
	auto n1 = Vec2f{-0.244, 1};
	auto n2 = Vec2f{4.712, 1};
	auto n3 = Vec2f{-0.647, 1};

	auto o1 = Vec2f{2, 0.75};
	auto o2 = Vec2f{5, 1};
	auto o3 = Vec2f{6, 0.75};

	nytl::normalize(n1);
	nytl::normalize(n2);
	nytl::normalize(n3);

	for(int x = 0; x <= 7; ++x) {
		for(int y = 0; y <= 2; ++y) {
			auto p = Vec2f{float(x), float(y)};
			float dd = length(o1 - p);
			float d = dist(p, o1, n1);
			int i = 1;

			if(length(o2 - p) < dd) {
				dd = length(o2 - p);
				d = dist(p, o2, n2);
				i = 2;
			}

			if(length(o3 - p) < dd) {
				dd = length(o3 - p);
				d = dist(p, o3, n3);
				i = 3;
			}

			dlg_info("{}, {}: {} (nearest: {})", x, y, d, i);
		}
	}
}
