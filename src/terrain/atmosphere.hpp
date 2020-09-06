#pragma once

#include <tkn/types.hpp>
#include <tkn/pipeline.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>

using namespace tkn::types;

struct AtmosphereLayer {
	float expTerm;
	float expScale;
	float lienarTerm;
	float constantTerm;
};

struct AtmosphereDesc {
	static constexpr auto maxLayers = 6u;

	float bottom; // lowest radius
	float top; // highest radius
	float sunAngularRadius;
	// The cosine of the maximum Sun zenith angle for which atmospheric scattering
	// must be precomputed (for maximum precision, use the smallest Sun zenith
	// angle yielding negligible sky light radiance values. For instance, for the
	// Earth case, 102 degrees is a good choice - yielding minMuS = -0.2).
	float minMuS;

	float mieG;
	float ozonePeak;
	float ozoneWidth;
	u32 nLayers;

	std::array<AtmosphereLayer, maxLayers> layers;

	Vec4f mieAbsorption;
	Vec4f ozoneAbsorption;
	Vec4f mieScattering;
	Vec4f rayleighScattering; // same as rayleighExtinction
	Vec4f solarIrradiance;
	Vec4f groundAlbedo;
};

class Atmosphere {
public:
	bool changed() const { return changed_; }
	void compute(vk::CommandBuffer cb);

	const AtmosphereDesc& desc() const { return desc_; }
	void desc(const AtmosphereDesc&) const;

	Vec3f sunLight() const { return sunLight_; }
	Vec3f skyLight() const { return skyLight_; }

private:
	vpp::ViewableImage multiscatLUT_;
	vpp::ViewableImage transmittanceLUT_;
	vpp::SubBuffer ubo_;
	vpp::SubBuffer writebackBuf_;
	AtmosphereDesc desc_;

	// Single compute pipe that can compute everything.
	// Computation mode chosen depending on push constant.
	tkn::ReloadablePipeline pipe_;

	std::vector<Vec3f> sunLight_;
	std::vector<Vec3f> skyLight_;
	bool changed_ {true};
};
