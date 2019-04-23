#pragma once

#include <vpp/fwd.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <nytl/mat.hpp>

namespace doi {

class Skybox {
public:
	void init(vpp::Device& dev, vk::RenderPass rp,
		unsigned subpass, vk::SampleCountBits samples);
	void updateDevice(const nytl::Mat4f& viewProj);
	void render(vk::CommandBuffer cb);

protected:
	vpp::Device* dev_;
	vpp::ViewableImage cubemap_;
	vpp::Sampler sampler_;
	vpp::SubBuffer indices_;
	vpp::SubBuffer ubo_;

	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

} // namespace doi
