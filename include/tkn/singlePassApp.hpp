#pragma once
#include <tkn/app.hpp>

namespace tkn {

// Implementation of App for projects that only use a single render pass.
// Optionally supports multisampling (via arguments and supportsMultisampling())
// and a depth buffer (see needsDepth()).
class SinglePassApp : public App {
protected:
	using App::init;
	bool doInit(nytl::Span<const char*> args, Args& out) override;
	void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) override;
	void record(const RenderBuffer&) override;
	argagg::parser argParser() const override;
	bool handleArgs(const argagg::parser_results&, Args& out) override;

	// Whether this app needs a depth buffer in the render pass.
	// Even if it doesn't need depth, it can return true here. But setting
	// this to false when not needed might increase performance.
	// Must always return the same value and not depend on App initialization.
	virtual bool needsDepth() const { return true; }

	// Whether this apps support multisampling. If this is overwritten
	// to return false, will not expose the multisampling command line
	// option. Must always return the same value and not depend on
	// App initialization.
	virtual bool supportsMultisampling() const { return true; }

	// Creates the default render pass
	virtual vpp::RenderPass createRenderPass();

	// Called by the default record implemention when starting
	// the renderpass instance.
	virtual std::vector<vk::ClearValue> clearValues();

	// Called by the default record implementation during the
	// renderpass instance.
	virtual void render(vk::CommandBuffer) {}

	// Called by the default record implementation before starting
	// the renderpass instance.
	virtual void beforeRender(vk::CommandBuffer) {}

	// Called by the default record implementation after finishing
	// the renderpass instance.
	virtual void afterRender(vk::CommandBuffer) {}

	using App::rvgInit;
	virtual void rvgInit(unsigned subpass = 0) {
		App::rvgInit(renderPass(), subpass, samples());
	}

	virtual vpp::ViewableImage createDepthTarget(const vk::Extent2D&);
	virtual vpp::ViewableImage createMultisampleTarget(const vk::Extent2D&);

	vk::RenderPass renderPass() const { return rp_; }
	const auto& multisampleTarget() const { return msTarget_; }
	const auto& depthTarget() const { return depthTarget_; }
	vk::SampleCountBits samples() const { return samples_; }
	vk::Format depthFormat() const { return depthFormat_; }

protected:
	vpp::RenderPass rp_;
	vpp::ViewableImage msTarget_;
	vpp::ViewableImage depthTarget_;
	vk::Format depthFormat_ {};
	vk::SampleCountBits samples_ {};
};

} // namespace tkn
