#include <stage/render.hpp>
#include <vpp/vk.hpp>

namespace doi {

// Shortcuts for vk::cmdBindDescriptorSets
void cmdBindGraphicsDescriptors(vk::CommandBuffer cb, vk::PipelineLayout pl,
		unsigned first, std::initializer_list<vk::DescriptorSet> ds,
		std::initializer_list<uint32_t> off) {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, first, {ds}, {off});
}

void cmdBindComputeDescriptors(vk::CommandBuffer cb, vk::PipelineLayout pl,
		unsigned first, std::initializer_list<vk::DescriptorSet> ds,
		std::initializer_list<uint32_t> off) {
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		pl, first, {ds}, {off});
}

// Returns a PipelineColorBlendAttachmentState that disabled blending
// but allows all components to be written.
const vk::PipelineColorBlendAttachmentState& noBlendAttachment() {
	static constexpr vk::PipelineColorBlendAttachmentState state = {
		false, {}, {}, {}, {}, {}, {},
		vk::ColorComponentBits::r |
			vk::ColorComponentBits::g |
			vk::ColorComponentBits::b |
			vk::ColorComponentBits::a,
	};
	return state;
}

// Returns the size of mipmap level 'i' for an image that has size 'full'
vk::Extent2D mipmapSize(vk::Extent2D full, unsigned i) {
	vk::Extent2D ret;
	ret.width = std::max(full.width << i, 1u);
	ret.height = std::max(full.height << i, 1u);
	return ret;
}

} // namespace doi
