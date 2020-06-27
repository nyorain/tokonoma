#include <tkn/scene/pbr.hpp>
#include <tkn/scene/environment.hpp>
#include <tkn/color.hpp>
#include <tkn/transform.hpp>
#include <tkn/sky.hpp>
#include <dlg/dlg.hpp>
#include <nytl/approxVec.hpp>
#include "bugged.hpp"

#include <sky/precoscat.hpp>
#include <random>

using nytl::approx;
using namespace nytl::approxOps;

std::pair<nytl::Mat2f, nytl::Vec2i> flipUVMatrix(uint face0, uint face1) {
	// all of these values are in {-1, 0, 1}
	auto f0 = tkn::cubemap::faces[face0];
	auto f1 = tkn::cubemap::faces[face1];

    float ss = dot(f0.s, f1.s);
    float st = dot(f0.s, f1.t);
    float ts = dot(f0.t, f1.s);
    float tt = dot(f0.t, f1.t);

    float d0s1 = dot(f0.dir, f1.s);
    float d0t1 = dot(f0.dir, f1.t);

    float d1s0 = dot(f1.dir, f0.s);
    float d1t0 = dot(f1.dir, f0.t);

	float dsds = d0s1 * d1s0;
	float dsdt = d0s1 * d1t0;
	float dtdt = d0t1 * d1t0;
	float dtds = d0t1 * d1s0;

	// TODO: might be possible to further optimize this
	// off values are one of {-1, 0, 1, 2}
	auto off = nytl::Vec2i{0, 0};
	off.x += int(abs(dsds) * (int(d0s1 == d1s0) + d1s0));
	off.x += int(abs(dtds) * (int(d0t1 == d1s0) + d1s0));
	off.x += (ss == -1 || st == -1) ? 1 : 0;

	off.y += int(abs(dtdt) * (int(d0t1 == d1t0) + d1t0));
	off.y += int(abs(dsdt) * (int(d0s1 == d1t0) + d1t0));
	off.y += (ts == -1 || tt == -1) ? 1 : 0;

	// integer matrix
	return {nytl::Mat2f{
		ss - dsds, st - dtds,
		ts - dsdt, tt - dtdt
		// ss - dsds, ts - dtds,
		// st - dsdt, tt - dtdt
	}, off};
}

TEST(cube) {
	auto [m1, o1] = flipUVMatrix(5, 3);
	dlg_info(m1);
	dlg_info(o1);
	// dlg_info(faceFromDir())
}

/*
TEST(A) {
	// sunny 16
	tkn::PBRCamera cam;
	cam.shutterSpeed = 1 / 100.f;
	cam.iso = 100.f;
	cam.aperture = 16.f;

	dlg_info(exposure(cam));
}

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
