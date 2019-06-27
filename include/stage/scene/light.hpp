#pragma once

#include <stage/scene/primitive.hpp>
#include <stage/types.hpp>
#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <vpp/handles.hpp>
#include <vpp/image.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>

// TODO: allow to configure dir light view frustum size
// TODO: allow to configure/change shadowmap sizes

namespace doi {

class Scene;
struct Camera;

struct ShadowData {
	vk::Format depthFormat;
	vpp::Sampler sampler;
	vpp::RenderPass rpPoint;
	vpp::RenderPass rpDir;
	vpp::PipelineLayout pl;
	vpp::Pipeline pipe;
	vpp::Pipeline pipeCube;
	bool multiview;

	// TODO: fine tune depth bias
	// should be scene dependent, configurable!
	// also dependent on shadow map size (the larger the shadow map, the
	// smaller are the values we need)
	float depthBias {1.f};
	float depthBiasSlope {8.f};
};

constexpr std::uint32_t lightFlagPcf = (1u << 0);
constexpr std::uint32_t lightFlagShadow = (1u << 1);

class DirLight {
public:
	// TODO: make variable? could be useful in some cases
	static constexpr u32 cascadeCount = 4u;

	struct {
		nytl::Vec3f color {1.f, 1.f, 1.f};
		std::uint32_t flags {lightFlagPcf | lightFlagShadow};
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
	const auto& ds() const { return ds_; }
	vk::ImageView shadowMap() const { return target_.vkImageView(); }
	// nytl::Mat4f lightBallMatrix(nytl::Vec3f viewPos) const;
	// const Primitive& lightBall() const { return lightBall_; }

protected:
	nytl::Vec2ui size_ {1024, 1024};
	vpp::ViewableImage target_; // depth
	vpp::Framebuffer fb_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;
	// Primitive lightBall_;

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
		std::uint32_t flags {lightFlagPcf};
		nytl::Vec3f position {1.f, 1.f, 1.f};
		float _; // padding
		// TODO: not really needed anymore in default pbr pipeline, using
		// only the radius. Might be useful for artistic effects though
		nytl::Vec3f attenuation {1.f, 4, 8};
		float radius {2.f};
	} data;

public:
	PointLight() = default;
	PointLight(const WorkBatcher&, const vpp::TrDsLayout& lightLayout,
		const vpp::TrDsLayout& primitiveLayout, const ShadowData& data,
		unsigned id, vk::ImageView noShadowMap = {});

	// renders shadow map
	void render(vk::CommandBuffer cb, const ShadowData&, const Scene&);
	void updateDevice();
	nytl::Mat4f lightBallMatrix() const;
	const auto& ds() const { return ds_; }
	vk::ImageView shadowMap() const { return shadowMap_.vkImageView(); }
	// const Primitive& lightBall() const { return lightBall_; }
	bool hasShadowMap() const { return data.flags & lightFlagShadow; }

protected:
	nytl::Vec2ui size_ {256u, 256u}; // per side
	vpp::ViewableImage shadowMap_; // cube map
	vpp::Framebuffer fb_;
	vpp::SubBuffer ubo_;
	vpp::TrDs ds_;
	// Primitive lightBall_;

	// non-multiview
	struct Face {
		vpp::ImageView view;
		vpp::Framebuffer fb;
	};
	std::vector<Face> faces_;
};

ShadowData initShadowData(const vpp::Device&, vk::Format depthFormat,
	vk::DescriptorSetLayout lightDsLayout, vk::DescriptorSetLayout sceneDsLayout,
	bool multiview, bool depthClamp);

} // namespace doi
