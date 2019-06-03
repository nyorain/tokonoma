#pragma once

#include <stage/scene/primitive.hpp>
#include <stage/types.hpp>
#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <vpp/handles.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>

// TODO: we currently have a hardcoded depth bias guess
// TODO: allow to configure dir light view frustum size
// TODO: allow to configure/change size_
//  probably based on light radius

namespace doi {

class Scene;
class Camera;

struct ShadowData {
	vk::Format depthFormat;
	vpp::Sampler sampler;
	vpp::RenderPass rp;
	vpp::RenderPass rpDir; // multiview
	vpp::PipelineLayout pl;
	vpp::Pipeline pipe;
	vpp::Pipeline pipeCube;
	bool multiview;
};

// TODO: enum or something?
constexpr std::uint32_t lightFlagDir = (1u << 0);
constexpr std::uint32_t lightFlagPcf = (1u << 1);
constexpr std::uint32_t lightFlagShadow = (1u << 2);

// order:
// front (topleft, topright, bottomleft, bottomright)
// back (topleft, topright, bottomleft, bottomright)
using Frustum = std::array<nytl::Vec3f, 8>;
Frustum ndcFrustum(); // frustum in ndc space, i.e. [-1, 1]^3

class DirLight {
public:
	// TODO: make variable
	static constexpr u32 cascadeCount = 4u;

	struct {
		nytl::Vec3f color {1.f, 1.f, 1.f};
		std::uint32_t flags {lightFlagDir | lightFlagPcf};
		nytl::Vec3f dir {1.f, 1.f, 1.f};
		float _ {}; // padding
	} data;

public:
	DirLight() = default;
	DirLight(const WorkBatcher&, const vpp::TrDsLayout& matDsLayout,
		const vpp::TrDsLayout& primitiveDsLayout, const ShadowData& data,
		unsigned id);

	// renders shadow map
	void render(vk::CommandBuffer cb, const ShadowData&, const Scene&);
	void updateDevice(const Camera& camera);
	// nytl::Mat4f lightBallMatrix(nytl::Vec3f viewPos) const;
	const auto& ds() const { return ds_; }
	vk::ImageView shadowMap() const { return target_.vkImageView(); }
	const Primitive& lightBall() const { return lightBall_; }

protected:
	nytl::Vec2ui size_ {512, 512};
	vpp::ViewableImage target_; // depth
	vpp::Framebuffer fb_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;
	Primitive lightBall_;

	// non-multiview
	struct Cascade {
		vpp::ImageView view;
		vpp::Framebuffer fb;
	};
	std::vector<Cascade> cascades_;
};

class PointLight {
public:
	struct {
		nytl::Vec3f color {1.f, 1.f, 1.f};
		std::uint32_t flags {0};
		nytl::Vec3f position {1.f, 1.f, 1.f};
		float _; // padding
		// TODO: params.x (constant term) is basically always 1.f, get rid
		// of that everywhere?
		// TODO: always automatically calculate attenuation from radius?
		nytl::Vec3f attenuation {1.f, 4, 8};
		float radius {2.f};
	} data;

public:
	PointLight() = default;
	PointLight(const WorkBatcher&, const vpp::TrDsLayout& matLayout,
		const vpp::TrDsLayout& primitiveLayout, const ShadowData& data,
		unsigned id, vk::ImageView noShadowMap = {});

	// renders shadow map
	void render(vk::CommandBuffer cb, const ShadowData&, const Scene&);
	void updateDevice();
	nytl::Mat4f lightMatrix(unsigned) const;
	nytl::Mat4f lightBallMatrix() const;
	const auto& ds() const { return ds_; }
	vk::ImageView shadowMap() const { return shadowMap_.vkImageView(); }
	const Primitive& lightBall() const { return lightBall_; }
	bool hasShadowMap() const { return data.flags & lightFlagShadow; }

protected:
	nytl::Vec2ui size_ {256u, 256u}; // per side
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
	vk::PushConstantRange materialPcr, bool multiview, bool depthClamp);

} // namespace doi
