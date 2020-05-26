#include <tkn/scene/pbr.hpp>
#include <dlg/dlg.hpp>
#include "bugged.hpp"

TEST(A) {
	// sunny 16
	tkn::PBRCamera cam;
	cam.shutterSpeed = 1 / 100.f;
	cam.iso = 100.f;
	cam.aperture = 16.f;

	dlg_info(exposure(cam));
}
