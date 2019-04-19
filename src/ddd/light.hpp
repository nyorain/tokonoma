#pragma once

#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <vpp/framebuffer.hpp>
#include <vpp/image.hpp>
#include <vpp/renderPass.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <stage/scene/scene.hpp>

// TODO:
// shadow mapping badly implemented. Still some artefacts, mixing up
// point and dir light; no support for point light: shadow cube map

struct ShadowData {
	vpp::Pipeline pipe;
	vpp::RenderPass rp;
	vpp::Sampler sampler;
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
		vk::Format depthFormat, vk::RenderPass rp);

	// renders shadow map
	void render(vk::CommandBuffer cb, vk::PipelineLayout,
		const ShadowData& data, const doi::Scene& scene);
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
};

ShadowData initShadowData(const vpp::Device&, vk::PipelineLayout, vk::Format);
