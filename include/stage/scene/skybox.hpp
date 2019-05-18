#pragma once

#include <stage/texture.hpp>
#include <stage/types.hpp>
#include <vpp/fwd.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <nytl/vec.hpp>
#include <nytl/stringParam.hpp>

namespace doi {

/// Indices to create a box that has it's front faces on the inside,
/// (as needed for a skybox) with the skybox.vert shader that generates
/// the positions.
constexpr std::array<u16, 36> boxInsideIndices = {
	0, 1, 2,  2, 1, 3, // front
	1, 5, 3,  3, 5, 7, // right
	2, 3, 6,  6, 3, 7, // top
	4, 0, 6,  6, 0, 2, // left
	4, 5, 0,  0, 5, 1, // bottom
	5, 4, 7,  7, 4, 6, // back
};

class Skybox {
public:
	void init(const WorkBatcher& wb, nytl::StringParam hdrFile,
		vk::RenderPass rp, unsigned subpass, vk::SampleCountBits samples);
	void init(const WorkBatcher& wb, vk::RenderPass rp,
		unsigned subpass, vk::SampleCountBits samples);
	void updateDevice(const nytl::Mat4f& viewProj);

	// Requires caller to bind cube index buffer
	void render(vk::CommandBuffer cb);
	// auto& indexBuffer() const { return indices_; }

	auto& skybox() const { return cubemap_; }
	auto& irradiance() const { return irradiance_; }

protected:
	void writeDs();
	void initPipeline(const vpp::Device&, vk::RenderPass rp, unsigned subpass,
		vk::SampleCountBits samples);

protected:
	const vpp::Device* dev_;
	doi::Texture cubemap_;
	vpp::Sampler sampler_;
	// vpp::SubBuffer indices_;
	vpp::SubBuffer ubo_;

	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;

	vpp::ViewableImage irradiance_;
};

} // namespace doi
