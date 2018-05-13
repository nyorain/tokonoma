#pragma once

#include <stage/geometry.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/framebuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/renderPass.hpp>
#include <vpp/image.hpp>
#include <nytl/vec.hpp>

class LightSystem;

struct ShadowSegment {
	doi::Segment2f seg;
	float opacity;
};

struct ShadowVertex {
	nytl::Vec2f pos;
	float opacity;
};

class Light {
public:
	bool valid {true};
	nytl::Vec4f color {1.f, 1.f, 1.f, 1.f};
	nytl::Vec2f position {};
	float radius {0.02};
	float strength {1.f};

public:
	Light(LightSystem& system);

	bool updateDevice();
	const auto& buffer() const { return buffer_; }
	const auto& lightDs() const { return lightDs_.vkHandle(); }
	const auto& shadowDs() const { return shadowDs_.vkHandle(); }
	const auto& renderTarget() const { return shadowTarget_; }
	const auto& framebuffer() const { return framebuffer_; }

protected:
	void writeUBO(nytl::Span<std::byte>& data);
	void init(LightSystem& system);

protected:
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
	Light& addLight();
	bool removeLight(Light&); // TODO: not working

	void update(double delta);
	bool updateDevice();

	auto& device() const { return dev_; }
	const auto& shadowDsLayout() const { return shadowDsLayout_; }
	const auto& lightDsLayout() const { return lightDsLayout_; }
	const auto& shadowPass() const { return shadowPass_; }
	const auto& lightPass() const { return lightPass_; }
	const auto& lightPipeLayout() const { return lightPipeLayout_; }
	const auto& renderTarget() const { return renderTarget_; }

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
	std::vector<Light> lights_;
};
