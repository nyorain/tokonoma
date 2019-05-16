#pragma once

#include <stage/texture.hpp>
#include <vpp/fwd.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/mat.hpp>
#include <nytl/vec.hpp>
#include <nytl/stringParam.hpp>

namespace doi {

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
