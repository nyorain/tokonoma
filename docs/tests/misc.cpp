#include <tkn/scene/pbr.hpp>
#include <tkn/scene/environment.hpp>
#include <tkn/color.hpp>
#include <tkn/sky.hpp>
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

/*
TEST(B) {
	auto turb = 8.0;
	auto ground = nytl::Vec3f{1.f, 1.f, 1.f};
	auto toSun = nytl::normalized(nytl::Vec3f{0.f, 1.f, 0.f});

	auto ra = tkn::hosekSky::sunRadianceRGB(1.0, turb);
	// auto sra = tkn::hosekSky::sunIrradianceRGB(1.0, turb);
	auto conf = tkn::hosekSky::bakeConfiguration(turb, ground, toSun);
	auto sky = tkn::XYZtoRGB(tkn::hosekSky::eval(conf, 1.0, 0.0, 1.0));

	dlg_info("combined irr: {}", 0.0000679 * (ra + sky));
	// dlg_info("sra: {}", 0.001f * ra);

	auto baked = tkn::Sky::bake(toSun, ground, turb);
	dlg_info(baked.sunIrradiance);
}
*/
