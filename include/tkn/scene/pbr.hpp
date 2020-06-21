#pragma once

#include <vpp/handles.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/vec.hpp>

// Just a collection of various utilities useful for pbr.
// TODO: Some of them should probably be split into their own files.
//   especially all the mapper/baker are not really related to pbr per se,
//   they should in something like 'bake.hpp' or 'gen.hpp'.

namespace tkn {

/// Creates a cubemap from an equirect environment map.
class Cubemapper {
public:
	void init(vpp::DeviceMemoryAllocator&, const nytl::Vec2ui& faceSize,
		vk::Sampler linear);
	void record(vk::CommandBuffer cb, vk::ImageView equirect);

	/// Returns (moves) the cubemap.
	/// The map has format r16g16b16a16Sfloat and is in general layout
	/// (after the recorded command buffer finishes).
	/// You will have to insert a barrier, the image was written in compute
	/// shader stage.
	/// Does not free internal data, move assign empty object for that.
	vpp::ViewableImage finish();

protected:
	static constexpr auto groupDimSize = 16u;

	vpp::ViewableImage::InitData initCubemap_;
	vpp::TrDs::InitData initDs_;

	nytl::Vec2ui size_;
	vpp::ViewableImage cubemap_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_; // compute
};

/// Creates irradiance maps form environment maps.
class Irradiancer {
public:
	void init(vpp::DeviceMemoryAllocator&, const nytl::Vec2ui& faceSize,
		vk::Sampler linear, float sampleDelta = 0.01);
	void record(vk::CommandBuffer cb, vk::ImageView environment);

	/// Returns (moves) the irradiance map.
	/// The map has format r16g16b16a16Sfloat and is in general layout (after
	/// the recorded command buffer finishes).
	/// You will have to insert a barrier, the image was written in compute
	/// shader stage.
	/// Does not free internal data, move assign empty object for that.
	vpp::ViewableImage finish();

protected:
	static constexpr auto groupDimSize = 8u;

	vpp::ViewableImage::InitData initIrradiance_;
	vpp::TrDs::InitData initDs_;

	nytl::Vec2ui size_;
	vpp::ViewableImage irradiance_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_; // compute
};

// TODO: can probably be just a functions that returns staging
// objects? Otherwise separate init and run methods for quick
// back processing.
// TODO(perf): consider just copying the first level (or ignoring
// it alltogether here) since it's the same in filtered and env map.

/// Prefilters an environment map for specular ibl.
/// Renders them as mipmap levels onto the cubemap.
class EnvironmentMapFilter {
public:
	struct Mip {
		vpp::ImageView view;
		vpp::TrDs ds;
	};

public:
	void create(const vpp::Device& dev, vk::Sampler linear,
		unsigned sampleCount = 1024);

	/// Will record the command to pre-convolute a given cubemap into
	/// the levels needed for specular IBL.
	/// The given images must have r16g16b16a16Sfloat format.
	/// - cubemap: The input cubemap. Should have a full valid
	///   mip chain, needed while sampling. Must be in shaderReadOnlyOptimal
	///   layout.
	/// - filtered: The output environment map with filtered levels.
	///   Should be in general layout. Must have 'mipLevels' number
	///   of levels. All content will be overwritten.
	/// - size: The size of the first level in 'filtered', i.e. the output.
	/// - linear: A linear sampler, used to filter the cubemap.
	///   Should allow filtering all mip levels and not have a mip level bias.
	/// - sampleCount: The number of samples to take per output pixel.
	///   Setting this higher will avoid artifacts.
	/// The returned vector of miplevel data must stay valid until
	/// the command buffer has finished execution.
	[[nodiscard]] std::vector<Mip> record(vk::CommandBuffer cb,
		vk::ImageView cubemap, vk::Image filtered,
		unsigned mipLevels, nytl::Vec2ui size, unsigned baseLayer = 0u);

protected:
	static constexpr auto groupDimSize = 8u;

	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_; // compute
};

/// Projects cubemaps to sphere harmonics (up to l=2, i.e. 9 coefficients).
/// Requires a cubemap 32x32-per-face image as input.
/// TODO: allow arbitrary sizes.
class SHProjector {
public:
	void create(const vpp::Device& dev, vk::Sampler linear);
	void record(vk::CommandBuffer, vk::ImageView irradianceCube);
	vpp::BufferSpan coeffsBuffer() const { return dst_; }

protected:
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::TrDs ds_;
	vpp::SubBuffer dst_;
};

// Physically based camera lens and shutter.
// ISO value is interpreted as saturation-based sensitivity.
struct PBRCamera {
	float shutterSpeed; // in seconds, t
	float iso; // iso value: e.g. 100, 400, 1600, S
	float aperture; // f-stop, e.g. 16 for f/16, N
};

// Returns the exposure value (EV100) for given aperature and shutter speed.
// See https://en.wikipedia.org/wiki/Exposure_value
// - N: f-stop number
// - t: shutterSpeed time (in seconds)
[[nodiscard]] inline float ev100(float N, float t) {
	return std::log2(N * N / t);
}

// Returns the EV100 value for the given camera.
[[nodiscard]] inline float ev100(struct PBRCamera& cam) {
	auto [t, S, N] = cam;
	return std::log2((N * N / t) * (100 / S));
}

// Returns an exposure value simulating the given camera.
// Basically a factor used to transform incoming illuminance to the
// captured luminance per pixel.
[[nodiscard]] inline float exposure(const PBRCamera& cam) {
	constexpr float q = 0.65; // capturing factor of the lens

	auto [t, S, N] = cam;
	float maxLum = 78 * N * N / (S * q * t);
	return 1.0 / maxLum;
}

} // namesapce tkn

