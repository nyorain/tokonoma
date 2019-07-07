#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <dlg/dlg.hpp>

using namespace nytl;

int main() {
	auto m = Mat3f {
		0, 4, 0,
		0, 0, 8,
		1, 1, 1
	};

	auto mm = nytl::inverse(m);
	// dlg_info(mm * Vec3f{3, 2, 1});
	// dlg_info(mm * Vec3f{1, 4, 1});
	dlg_info(mm * Vec3f{4, -4, 1});
}
