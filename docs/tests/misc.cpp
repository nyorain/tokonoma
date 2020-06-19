#include <tkn/scene/pbr.hpp>
#include <tkn/scene/environment.hpp>
#include <tkn/color.hpp>
#include <tkn/sky.hpp>
#include <dlg/dlg.hpp>
#include <nytl/approxVec.hpp>
#include "bugged.hpp"

#include <sky/precoscat.hpp>
#include <random>

using nytl::approx;
using namespace nytl::approxOps;

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

/*
TEST(C) {
	Atmosphere atmosphere;

	atmosphere.bottom = 6360000.0;
	atmosphere.top = 6420000.0;
	atmosphere.sunAngularRadius = 0.00935 / 2.0;
	atmosphere.minMuS = -0.2;
	atmosphere.mieG = 0.4f;

	std::random_device rd;
	std::default_random_engine e(rd());
	std::uniform_real_distribution<float> du(0.f, 1.f);

	// test that mapping and inverse make sense
	auto eps = 0.001;
	for(auto i = 0u; i < 5000u; ++i) {
		nytl::Vec4f unit{du(rd), du(rd), du(rd), du(rd)};

		// avoid numberic issues when the position is pretty much
		// exactly bottom of the atmosphere.
		// TODO: should probably be fixed, might lead to issues
		if(unit[3] < 0.1 || unit[2] < 0.1) {
			eps = 0.05;

			if(unit[3] < 0.05 || unit[2] < 0.05) {
				continue;
			}
		}

		// dlg_info("unit: {}", unit);

		ARay ray;
		float mu_s, nu;
		bool rayIntersectsGround;
		scatParamsFromTexUnit(atmosphere, unit, ray, mu_s, nu, rayIntersectsGround);
		// dlg_info("intersects: {}", rayIntersectsGround);

		EXPECT(mu_s <= 1.f && mu_s >= -1.f, true);
		EXPECT(nu <= 1.f && nu >= -1.f, true);
		EXPECT(ray.mu <= 1.f && ray.mu >= -1.f, true);
		EXPECT(ray.height <= atmosphere.top && ray.height >= atmosphere.bottom, true);

		auto unit2 = scatTexUnitFromParams(atmosphere, ray, mu_s, nu, rayIntersectsGround);
		// dlg_info("unit2: {}", unit2);
		EXPECT(unit, approx(unit2, eps));
	}

	// test 4D -> 3D mappings
	nytl::Vec4ui size{8u, 32u, 128u, 64u};
	nytl::Vec4f unit{du(rd), du(rd), du(rd), du(rd)};

}
*/
