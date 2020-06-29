#include "fit.hpp"
#include <memory>
#include <tkn/image.cpp>
#include <dlg/dlg.hpp>

// size of precomputed table (theta, alpha)
const int N = 64;

int main() {
    BrdfGGX brdf;

    // allocate data
    auto tab = std::make_unique<Mat3f[]>(N*N);
    auto tabMagFresnel = std::make_unique<Vec2f[]>(N*N);
    auto tabSphere = std::make_unique<float[]>(N*N);

    // fit
    fitTab(tab.get(), tabMagFresnel.get(), N, brdf);

    // projected solid angle of a spherical cap, clipped to the horizon
    genSphereTab(tabSphere.get(), N);

    // pack tables (texture representation)
    auto tex1 = std::make_unique<Vec4f[]>(N*N);
    auto tex2 = std::make_unique<Vec4f[]>(N*N);
    packTab(tex1.get(), tex2.get(),
		tab.get(), tabMagFresnel.get(), tabSphere.get(), N);

	// save as ktx
	auto ptr1 = reinterpret_cast<const std::byte*>(tex1.get());
	auto ptr2 = reinterpret_cast<const std::byte*>(tex2.get());
	auto provider = tkn::wrapImage({N, N, 1}, vk::Format::r32g32b32a32Sfloat,
		1, 2, {ptr1, ptr2});
	auto err = tkn::writeKtx("ltc.ktx", *provider);
	dlg_assert(err == tkn::WriteError::none);
	return err == tkn::WriteError::none;
}
