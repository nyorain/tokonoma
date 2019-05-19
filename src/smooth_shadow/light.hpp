#pragma once

#include <stage/geometry.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/handles.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/image.hpp>
#include <nytl/vec.hpp>

class LightSystem;

struct ShadowSegment {
	doi::Segment2f seg;
	float opacity;
};

/// Simple blackbody approxmiation.
/// Converts kelvin color temparature (1000K - 40000K) to a rbg color.
nytl::Vec3f blackbody(unsigned kelvin);

/// Computes the radius of the light bounds for a given
/// light radius and strength.
float lightBounds(float radius, float strength);

/// Computes the need shadow buffer size for a light with the given
/// bounds.
unsigned shadowBufSize(float bounds);

class Light {
public:
	nytl::Vec2f position;
	nytl::Vec4f color;

public:
	Light(LightSystem& system, nytl::Vec2f pos,
		float radius = 0.2, float strength = 1.f,
		nytl::Vec4f color = {1.f, 1.f, 0.8f, 1.f});

	bool updateDevice();
	void radius(float, bool recreate = true);
	void strength(float, bool recreate = true);

	float radius() const { return radius_; }
	float strength() const { return strength_; }
	float bounds() const { return bounds_; }
	unsigned bufSize() const { return bufSize_; }
	const auto& system() const { return system_; }

	const auto& buffer() const { return buffer_; }
	const auto& lightDs() const { return lightDs_.vkHandle(); }
	const auto& shadowDs() const { return shadowDs_.vkHandle(); }
	const auto& renderTarget() const { return shadowTarget_; }
	const auto& framebuffer() const { return framebuffer_; }

protected:
	void writeUBO(nytl::Span<std::byte>& data);
	void init();
	void createBuf();

protected:
	LightSystem& system_;

	bool recreate_ {};
	float radius_ {0.1f};
	float strength_ {1.f};
	float bounds_;
	unsigned bufSize_;

	vpp::Framebuffer framebuffer_;
	vpp::ViewableImage shadowTarget_;

	vpp::SubBuffer buffer_;
	vpp::TrDs shadowDs_;
	vpp::TrDs lightDs_;
};

/// Owns and manages all lights in a level.
class LightSystem {
public:
	LightSystem(vpp::Device& dev, vk::DescriptorSetLayout viewLayout);

	void renderLights(vk::CommandBuffer);
	void renderShadowBuffers(vk::CommandBuffer);

	void addSegment(const ShadowSegment&);
	Light& addLight(); // TODO: arguments
	bool removeLight(Light&); // TODO: not working
	bool updateDevice();

	auto& device() const { return dev_; }
	const auto& shadowDsLayout() const { return shadowDsLayout_; }
	const auto& lightDsLayout() const { return lightDsLayout_; }
	const auto& shadowPass() const { return shadowPass_; }
	const auto& lightPass() const { return lightPass_; }
	const auto& lightPipeLayout() const { return lightPipeLayout_; }
	const auto& renderTarget() const { return renderTarget_; }

	auto& lights() { return lights_; }
	auto& lights() const { return lights_; }

protected:
	vpp::Device& dev_;
	vk::DescriptorSetLayout viewLayout_;
	nytl::Vec2ui renderSize_;
	vpp::ViewableImage renderTarget_;
	vpp::Framebuffer framebuffer_;

	vpp::RenderPass lightPass_;
	vpp::RenderPass shadowPass_;

	vpp::Pipeline shadowPipe_;
	vpp::Pipeline lightPipe_;
	vpp::Sampler sampler_;

	vpp::TrDsLayout shadowDsLayout_;
	vpp::TrDsLayout lightDsLayout_;
	vpp::PipelineLayout shadowPipeLayout_;
	vpp::PipelineLayout lightPipeLayout_;

	std::vector<ShadowSegment> segments_;
	vpp::SubBuffer vertexBuffer_;
	std::deque<Light> lights_;
};
