#pragma once

#include <stage/scene/primitive.hpp>
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
// TODO: something about matrix configuration
// TODO: light ball visualization (primitive)
// TODO: allow to configure/change size_
//  probably based on light radius

namespace doi {

class Scene;

struct ShadowData {
	vk::Format depthFormat;
	vpp::Sampler sampler;
	vpp::RenderPass rp;
	vpp::PipelineLayout pl;
	vpp::Pipeline pipe;
	vpp::Pipeline pipeCube;
};

// TODO: enum or something?
constexpr std::uint32_t lightFlagDir = (1u << 0);
constexpr std::uint32_t lightFlagPcf = (1u << 1);

class DirLight {
public:
	struct {
		nytl::Vec3f color {1.f, 1.f, 1.f};
		std::uint32_t flags {lightFlagDir};
		nytl::Vec3f dir {1.f, 1.f, 1.f};
		float _ {}; // padding
	} data;

public:
	DirLight() = default;
	DirLight(const vpp::Device&, const vpp::TrDsLayout& matDsLayout,
		const vpp::TrDsLayout& primitiveDsLayout, const ShadowData& data,
		nytl::Vec3f viewPos, const Material& lightBallMat);

	// renders shadow map
	void render(vk::CommandBuffer cb, const ShadowData&, const Scene&);
	void updateDevice(nytl::Vec3f viewPos);
	nytl::Mat4f lightMatrix(nytl::Vec3f viewPos) const;
	nytl::Mat4f lightBallMatrix(nytl::Vec3f viewPos) const;
	const auto& ds() const { return ds_; }
	vk::ImageView shadowMap() const { return target_.vkImageView(); }
	const Primitive& lightBall() const { return lightBall_; }

protected:
	nytl::Vec2ui size_ {2048u, 2048u};
	vpp::ViewableImage target_; // depth
	vpp::Framebuffer fb_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;
	Primitive lightBall_;
};

class PointLight {
public:
	struct {
		nytl::Vec3f color {1.f, 1.f, 1.f};
		std::uint32_t flags {0};
		nytl::Vec3f position {1.f, 1.f, 1.f};
		float farPlane {30.f};
	} data;

public:
	PointLight() = default;
	PointLight(const vpp::Device&, const vpp::TrDsLayout& matLayout,
		const vpp::TrDsLayout& primitiveLayout, const ShadowData& data,
		const Material& lightBallMat);

	// renders shadow map
	void render(vk::CommandBuffer cb, const ShadowData&, const Scene&);
	void updateDevice();
	nytl::Mat4f lightMatrix(unsigned) const;
	nytl::Mat4f lightBallMatrix() const;
	const auto& ds() const { return ds_; }
	vk::ImageView shadowMap() const { return shadowMap_.vkImageView(); }
	const Primitive& lightBall() const { return lightBall_; }

protected:
	nytl::Vec2ui size_ {512u, 512u}; // per side
	vpp::ViewableImage target_; // normal depth buffer for rendering
	vpp::ViewableImage shadowMap_; // cube map
	vpp::Framebuffer fb_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;
	Primitive lightBall_;
};

ShadowData initShadowData(const vpp::Device&, vk::Format depthFormat,
	vk::DescriptorSetLayout lightDsLayout,
	vk::DescriptorSetLayout materialDsLayout,
	vk::DescriptorSetLayout primitiveDsLayout,
	vk::PushConstantRange materialPcr);

} // namespace doi
