#pragma once

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/framebuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/renderPass.hpp>
#include <vpp/image.hpp>
#include <nytl/vec.hpp>

class LightSystem;

struct ShadowSegment {
	kyo::Segment2f seg;
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
	float radius {0.04};
	float strength {1.f};

public:
	const auto& buffer() const { return buffer_; }
	const auto& lightDs() const { return lightDs_.vkHandle(); }
	const auto& shadowDs() const { return shadowDs_.vkHandle(); }
	const auto& renderTarget() const { return shadowTarget_; }
	const auto& framebuffer() const { return framebuffer_; }

protected:
	void writeUBO(std::byte*& data);
	void init(LightSystem& system);

protected:
	vpp::Framebuffer framebuffer_;
	vpp::ViewableImage shadowTarget_;

	vpp::SharedBuffer buffer_;
	vpp::TrDs shadowDs_;
	vpp::TrDs lightDs_;
};

/// Owns and manages all lights in a level.
class LightSystem {
public:
	LightSystem(const vpp::Device& dev);

	void renderLights(vk::CommandBuffer);
	void renderShadowBuffers(vk::CommandBuffer);

	Light& addLight();
	bool removeLight(Light&);

	void update(double delta);
	void updateDevice();

protected:
	nytl::Vec2ui renderSize;
	vpp::ViewableImage renderTarget;
	vpp::Framebuffer framebuffer;

	vpp::RenderPass renderPass; // should be called lightPass
	vpp::RenderPass shadowPass;

	vpp::Pipeline shadowPipe;
	vpp::Pipeline lightPipe;
	vpp::Pipeline mirrorLightPipe;
	vpp::Pipeline mirrorShadowPipe;
	vpp::Sampler sampler;

	vpp::TrDsLayout shadowDsLayout;
	vpp::TrDsLayout lightDsLayout;
	vpp::PipelineLayout shadowPipeLayout;
	vpp::PipelineLayout lightPipeLayout;

	std::vector<ShadowSegment> segments;
	vpp::SharedBuffer vertexBuffer;
	std::vector<Light> lights;
};
