#include <tkn/app2.hpp>

namespace tkn {

class SinglePassApp : public App {
protected:
	void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) override;
	void record(const RenderBuffer&) override;

	// Whether this app needs a depth buffer in the render pass.
	// Even if it doesn't need depth, it can return true here. But setting
	// this to false when not needed might increase performance.
	virtual bool needsDepth() const { return true; }

	// Creates the default render pass
	virtual vpp::RenderPass createRenderPass();

	// Called by the default App::record implemention when starting
	// the renderpass instance.
	virtual std::vector<vk::ClearValue> clearValues();

	// Called by the default App::record implementation during the
	// renderpass instance.
	virtual void render(vk::CommandBuffer) {}

	// Called by the default App::record implementation before starting
	// the renderpass instance.
	virtual void beforeRender(vk::CommandBuffer) {}

	// Called by the default App::record implementation after finishing
	// the renderpass instance.
	virtual void afterRender(vk::CommandBuffer) {}

	virtual vpp::ViewableImage createDepthTarget(const vk::Extent2D&);
	virtual vpp::ViewableImage createMultisampleTarget(const vk::Extent2D&);

protected:
	vpp::RenderPass rp;
	vpp::ViewableImage msTarget;
	vpp::ViewableImage depthTarget;
	vk::SampleCountBits samples;
};

} // namespace tkn
