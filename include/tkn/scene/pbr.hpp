#pragma once

#include <vpp/handles.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/vec.hpp>

// just a collection of various utilities useful for pbr.
// should probably be split into their own files

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

// TODO: filter from mipmaps to avoid artefacts
// TODO: can probably be just a functions that returns staging
// objects? Otherwise seperate init and run methods

/// Prefilters an environment map for specular ibl.
/// Renders them as mipmap levels onto the cubemap.
class EnvironmentMapFilter {
public:
	/// The given image must have r16g16b16a16Sfloat format.
	/// The first mip level must be in shaderReadOnlyOptimal layout,
	/// all others in general layout. Layouts won't be changed.
	void record(const vpp::Device& dev, vk::CommandBuffer cb,
		vk::Image envImage, vk::ImageView envView, vk::Sampler linear,
		unsigned mipLevels, nytl::Vec2ui size, unsigned sampleCount = 4096);

protected:
	static constexpr auto groupDimSize = 8u;

	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_; // compute

	struct Mip {
		vpp::ImageView view;
		vpp::TrDs ds;
	};

	std::vector<Mip> mips_;
};

/// Projects cubemaps to sphere harmonics (up to l=2, i.e. 9 coefficients).
/// Requires a cubemap 32x32-per-face image as input.
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

} // namesapce tkn

