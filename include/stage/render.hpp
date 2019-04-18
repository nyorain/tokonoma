// Copyright (c) 2017 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

#pragma once

#include <vpp/device.hpp> // vpp::Device
#include <vpp/queue.hpp> // vpp::Queue
#include <vpp/buffer.hpp> // vpp::Buffer
#include <vpp/renderer.hpp> // vpp::DefaultRenderer
#include <vpp/commandBuffer.hpp>
#include <vpp/renderPass.hpp>
#include <vpp/framebuffer.hpp>
#include <vpp/queue.hpp>
#include <vpp/vk.hpp>
#include <nytl/vec.hpp>

namespace doi {

struct RendererCreateInfo {
	const vpp::Device& dev;
	vk::SurfaceKHR surface;
	nytl::Vec2ui size;
	const vpp::Queue& present;

	vk::SampleCountBits samples = vk::SampleCountBits::e1;
	bool vsync = false;
	std::array<float, 4> clearColor = {0.f, 0.f, 0.f, 1.f};
	bool depth = false;
};

// Fairly simple renderer implementation that supports one pass,
// optionally with depth and/or mutlisample target.
class Renderer : public vpp::Renderer {
public:
	std::function<void(vk::CommandBuffer)> beforeRender;
	std::function<void(vk::CommandBuffer)> onRender;
	std::function<void(vk::CommandBuffer)> afterRender;

public:
	Renderer(const RendererCreateInfo& info);
	~Renderer() = default;

	virtual void resize(nytl::Vec2ui size);
	virtual void samples(vk::SampleCountBits);

	// Sets the new clear color.
	// Note that the new color will only be used on next rerecord.
	virtual void clearColor(std::array<float, 4> newColor) {
		clearColor_ = newColor;
	}

	vk::RenderPass renderPass() const { return renderPass_; }
	vk::SampleCountBits samples() const { return sampleCount_; }
	vk::Format depthFormat() const { return depthFormat_; }

protected:
	// for deriving:
	Renderer(const vpp::Queue& present);
	virtual void init(const RendererCreateInfo& info);
	virtual void createRenderPass();

	virtual void createMultisampleTarget(const vk::Extent2D& size);
	virtual void createDepthTarget(const vk::Extent2D& size);
	void record(const RenderBuffer&) override;
	void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) override;

protected:
	vpp::ViewableImage multisampleTarget_;
	vpp::ViewableImage depthTarget_;
	vpp::RenderPass renderPass_;
	vk::SampleCountBits sampleCount_;
	vk::SwapchainCreateInfoKHR scInfo_;
	std::array<float, 4> clearColor_;
	vk::Format depthFormat_;
};

} // namespace doi
