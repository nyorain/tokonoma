#pragma once

#include <vpp/handles.hpp>
#include <vpp/image.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/vec.hpp>

namespace doi {

/// Creates irradiance maps form environment maps.
class Irradiancer {
public:
	static constexpr auto groupDimSize = 8u;

public:
	void init(vpp::DeviceMemoryAllocator&, const nytl::Vec2ui& size,
		vk::Sampler linear, float sampleDelta = 0.025);
	void record(vk::CommandBuffer cb, vk::ImageView environment);
	vpp::ViewableImage finish(); // move returns the irradiance map

protected:
	vpp::ViewableImage::InitData initIrradiance_;
	vpp::TrDs::InitData initDs_;

	nytl::Vec2ui size_;
	vpp::ViewableImage irradiance_;
	vpp::TrDs ds_;
	vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_; // compute
};

/// Prefilters an environment map for specular ibl.
/// Renders them as mipmap levels onto the cubemap.
class EnvironMapFilter {
public:
	void record(const vpp::Device& dev, vk::CommandBuffer cb,
		vk::Image envImage, vk::ImageView envView, unsigned mipLevels);

protected:
	vpp::ImageView mipViews_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_; // compute
};

} // namesapce doi

