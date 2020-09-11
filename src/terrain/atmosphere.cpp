#include "atmosphere.hpp"
#include <tkn/render.hpp>
#include <tkn/util.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>

// AtmosphereDesc
AtmosphereDesc AtmosphereDesc::earth(float scale) {
	AtmosphereDesc ret {};

	ret.bottom = 6'360'000 / scale;
	ret.top = 6'420'000 / scale;
	ret.nLayers = 3;
	ret.groundAlbedo = {0.3f, 0.3f, 0.3f};

	// rayleigh
	ret.layers[0].type = AtmosphereLayer::Type::exp;
	ret.layers[0].data[0] = 1.f;
	ret.layers[0].data[1] = -scale / 8000.f;

	ret.layers[0].scattering = scale * 1e-6f * Vec3f{5.8, 13.5, 33.1};
	ret.layers[0].g = 0.f;

	// mie
	ret.layers[1].type = AtmosphereLayer::Type::exp;
	ret.layers[1].data[0] = 1.f;
	ret.layers[1].data[1] = -scale / 1200.f;

	ret.layers[1].scattering = 5 * scale * 1e-6f * 3.996f * Vec3f{1.f, 1.f, 1.f};
	ret.layers[1].absorption = scale * 1e-6f * 0.4f * Vec3f{1.f, 1.f, 1.f};
	ret.layers[1].g = 0.8f;

	// ozone
	ret.layers[2].type = AtmosphereLayer::Type::tent;
	ret.layers[2].data[0] = 25'000.f / scale;
	ret.layers[2].data[1] = 15'000.f / scale;

	ret.layers[2].absorption = scale * 1e-6f * Vec3f{0.65, 1.88, 0.08};

	return ret;
}

// Atmosphere
Atmosphere::Atmosphere(const vpp::Device& dev, tkn::FileWatcher& fswatch,
		const AtmosphereDesc& desc) : desc_(desc) {

	auto info = vpp::ViewableImageCreateInfo(transmittanceFormat,
		vk::ImageAspectBits::color, transmittanceSize,
		vk::ImageUsageBits::sampled | vk::ImageUsageBits::storage);
	transmittanceLUT_ = {dev.devMemAllocator(), info, dev.deviceMemoryTypes()};
	ubo_ = {dev.bufferAllocator(), sizeof(UboData),
		vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
	uboMap_ = ubo_.memoryMap();

	pipe_ = {dev, "terrain/atmosphere.comp", fswatch};
	auto& dsu = pipe_.dsu();
	dsu(transmittanceLUT_);
	dsu.skip(2); // dsu(multiscatLUT_);
	dsu(ubo_);
}

void Atmosphere::compute(vk::CommandBuffer cb) {
	changed_ = false;

	// start barrier
	tkn::barrier(cb, transmittanceLUT_.image(),
		tkn::SyncScope::discard(),
		tkn::SyncScope::computeWrite());

	// compute
	tkn::cmdBind(cb, pipe_);

	auto gx = tkn::ceilDivide(transmittanceSize.width, 8u);
	auto gy = tkn::ceilDivide(transmittanceSize.height, 8u);
	vk::cmdDispatch(cb, gx, gy, 1u);

	// end barrier
	tkn::barrier(cb, transmittanceLUT_.image(),
		tkn::SyncScope::computeWrite(),
		tkn::SyncScope::allSampled());
}

void Atmosphere::desc(const AtmosphereDesc& desc) {
	desc_ = desc;
	changed_ = true;
}

bool Atmosphere::updateDevice() {
	auto& data = tkn::as<UboData>(uboMap_);
	data.atmosphere = desc_;
	data.sunIrradiance = {30.f, 30.f, 30.f};
	uboMap_.flush();

	if(pipe_.updateDevice()) {
		changed_ = true;
		return true;
	}

	return false;
}
