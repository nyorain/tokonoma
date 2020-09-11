#pragma once

#include <tkn/types.hpp>
#include <tkn/pipeline.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>

using namespace tkn::types;

struct AtmosphereLayer {
	enum class Type : u32 {
		exp = 0,
		linear = 1,
		tent = 2,
	};

	Vec3f data;
	Type type;

	Vec3f scattering;
	float g;
	Vec3f absorption;
	float _pad;
};

struct AtmosphereDesc {
	static constexpr auto maxLayers = 3u;
	static AtmosphereDesc earth(float scale = 1.f);

	float bottom; // lowest radius
	float top; // highest radius
	u32 nLayers;
	float _pad0;
	Vec3f groundAlbedo;
	float _pad1;
	std::array<AtmosphereLayer, maxLayers> layers;
};

class Atmosphere {
public:
	static constexpr auto transmittanceSize = vk::Extent2D{256u, 64u};
	static constexpr auto transmittanceFormat = vk::Format::r32g32b32a32Sfloat;

	struct UboData {
		AtmosphereDesc atmosphere;
		Vec3f sunIrradiance;
	};

public:
	Atmosphere() = default;
	Atmosphere(const vpp::Device&, tkn::FileWatcher&,
		const AtmosphereDesc& desc = AtmosphereDesc::earth());

	bool changed() const { return changed_; }
	void compute(vk::CommandBuffer cb);
	// void computeLight(vk::CommandBuffer cb, float r, float muS);

	void update() { pipe_.update(); }
	bool updateDevice();

	const AtmosphereDesc& desc() const { return desc_; }
	void desc(const AtmosphereDesc&);

	vk::ImageView transmittanceLUT() const { return transmittanceLUT_.vkImageView(); }
	vpp::BufferSpan ubo() const { return ubo_; }
	auto& pipe() const { return pipe_; }

	Vec3f sunLight() const { return sunLight_; }
	Vec3f skyLight() const { return skyLight_; }

	// TODO: precompute
	// Vec3f lookupSunLight(float r, float muS) const;
	// Vec3f lookupSkyLight(float r, float muS) const;

private:
	AtmosphereDesc desc_;

	vpp::ViewableImage multiscatLUT_;
	vpp::ViewableImage transmittanceLUT_;
	vpp::SubBuffer ubo_;
	vpp::MemoryMapView uboMap_;
	vpp::SubBuffer writebackBuf_;

	// Single compute pipe that can compute everything.
	// Computation mode chosen depending on push constant.
	tkn::ManagedComputePipe pipe_;

	Vec3f sunLight_;
	Vec3f skyLight_;

	// precomputed tables?
	// std::vector<Vec3f> sunLight_;
	// std::vector<Vec3f> skyLight_;
	// or textures?
	// vpp::ViewableImage sunLightTex_;
	// vpp::ViewableImage skyLightTex_;

	// Whether the settings have changed and recomputation is needed
	bool changed_ {true};
};
