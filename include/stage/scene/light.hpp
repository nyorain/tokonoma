#pragma once

#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <vpp/framebuffer.hpp>
#include <vpp/image.hpp>
#include <vpp/renderPass.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>

// TODO:
// shadow mapping badly implemented. Still some artefacts, light matrix
// and depth bias guessing, mixing up point and dir light;
// no support for point light: shadow cube map

namespace doi {

class Scene;

struct ShadowData {
	vk::Format depthFormat;
	vpp::Sampler sampler;
	vpp::RenderPass rp;
	vpp::PipelineLayout pl;
	vpp::Pipeline pipe;
};

struct Light {
	enum class Type : std::uint32_t {
		point = 1u,
		dir = 2u,
	};

	struct {
		nytl::Vec3f pd {1.f, 1.f, 1.f}; // position/direction
		Type type {Type::point};
		nytl::Vec3f color {1.f, 1.f, 1.f};
		std::uint32_t pcf {0};
	} data;
};

static_assert(sizeof(Light) == sizeof(float) * 8);

class DirLight : public Light {
public:
	DirLight() = default;
	DirLight(const vpp::Device&, const vpp::TrDsLayout&,
		const ShadowData& data);

	// renders shadow map
	void render(vk::CommandBuffer cb, const ShadowData&, const Scene&);
	void updateDevice();
	nytl::Mat4f lightMatrix() const;
	const auto& ds() const { return ds_; }
	vk::ImageView shadowMap() const { return target_.vkImageView(); }

protected:
	nytl::Vec2ui size_ {2048u, 2048u};
	vpp::ViewableImage target_; // depth
	vpp::Framebuffer fb_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;

	// TODO: something about matrix
	// TODO: light ball visualization (primitive)
};

ShadowData initShadowData(const vpp::Device&, vk::Format depthFormat,
	vk::DescriptorSetLayout lightDsLayout,
	vk::DescriptorSetLayout materialDsLayout,
	vk::DescriptorSetLayout primitiveDsLayout,
	vk::PushConstantRange materialPcr);

} // namespace doi
