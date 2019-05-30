#include "scatter.hpp"

void LightScatterPass::create(InitData&, const PassCreateInfo&,
		bool directional, SyncScope dstTarget) {
}

void LightScatterPass::init(InitData&, const PassCreateInfo&) {
}

void LightScatterPass::createBuffers(InitBufferData&, vk::Extent2D) {
}

void LightScatterPass::initBuffers(InitBufferData&, vk::Extent2D,
		vk::ImageView depth) {
	// target

	vpp::DescriptorSetUpdate dsu(ds_);
	dsu.imageSampler({{{}, depth, vk::ImageLayout::shaderReadOnlyOptimal}});
	dsu.uniform({{{ubo_}}});
	dsu.apply();
}

void LightScatterPass::record(vk::CommandBuffer cb, vk::Extent2D size,
		vk::DescriptorSet scene, vk::DescriptorSet light) {
	vk::cmdBeginRenderPass(cb, {
		rp_, fb_,
		{0u, 0u, size.width, size.height},
		0, nullptr
	}, {});

	doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {scene, ds_, light});
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
	vk::cmdDraw(cb, 4, 1, 0, 0); // tri fan fullscreen
	vk::cmdEndRenderPass(cb);
}

void LightScatterPass::updateDevice() {
	auto span = uboMap_.span();
	doi::write(span, params);
	uboMap_.flush();
}

SyncScope LightScatterPass::dstScopeDepth() const {
	return SyncScope::fragmentSampled();
}
