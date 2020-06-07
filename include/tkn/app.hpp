#pragma once

#include <tkn/file.hpp>
#include <tkn/shader.hpp>
#include <nytl/span.hpp>
#include <nytl/nonCopyable.hpp>

#include <ny/fwd.hpp>
#include <ny/event.hpp>
#include <vpp/fwd.hpp>
#include <vpp/renderer.hpp>
#include <vkpp/structs.hpp> // for features
#include <rvg/fwd.hpp>
#include <vui/fwd.hpp>

#include <memory>
#include <variant>
#include <string>
#include <optional>

namespace argagg { // fwd
	struct parser;
	struct parser_results;
} // namespace argagg

namespace tkn {

// fwd
class MainWindow;
class Renderer;
struct Features;

/// Implements basic setup and main loop.
class [[deprecated("will be removed, use tkn/app2 instead")]] App
	: public nytl::NonMovable {
public:
	using RenderBuffer = vpp::Renderer::RenderBuffer;

public:
	App();
	virtual ~App();

	virtual bool init(nytl::Span<const char*> args);
	virtual void run();

	ny::AppContext& appContext() const;
	MainWindow& window() const;

	vpp::Instance& vulkanInstance() const;
	vpp::Device& vulkanDevice() const;
	vpp::Device& device() const { return vulkanDevice(); }
	vpp::RenderPass& renderPass() const;
	vpp::DebugMessenger& debugMessenger() const;

	rvg::Context& rvgContext() const;
	rvg::Transform& windowTransform() const;
	vui::Gui& gui() const;

	vk::SampleCountBits samples() const;
	const vk::SwapchainCreateInfoKHR& swapchainInfo() const;
	rvg::Font& defaultFont() const;

	// might be invalid, depends on settings
	vpp::ViewableImage& depthTarget() const;
	vpp::ViewableImage& multisampleTarget() const;
	vk::Format depthFormat() const;

protected:
	// == Information methods: must return constant values ==
	// If overwritten by derived class to return true, will create default
	// framebuffer and renderpass with a depth buffer as attachment 1
	virtual bool needsDepth() const { return false; }

	// Should be overwritten to return the name of this app.
	virtual const char* name() const { return "tkn app"; }

	// Should return {} if rvg (and vui) are not needed, otherwise
	// the render and subpass they are used in.
	// NOTE: might be a limitation for some apps to only use rvg and vui
	// in one subpass, rvg requires it like that though. Changing it will
	// be hard since then rvg needs to compile multiple pipelines (one
	// for each render/subpass).
	virtual std::pair<vk::RenderPass, unsigned> rvgPass() const;

	virtual const char* usageParams() const { return "[options]"; }


	// argument parsing
	virtual argagg::parser argParser() const;
	virtual bool handleArgs(const argagg::parser_results&);

	// Called before device creation with the supported features
	// of the selected physical device (supported). Can be used to
	// enable the supported ones.
	virtual bool features(Features& enable, const Features& supported);


	// == Render stuff ==
	// Called after the vulkan device is initialized but before any
	// rendering data will be initialized (render pass, buffers etc).
	// Can be used to initialize static data such as layouts and samplers
	// that may be used in functions like initBuffers.
	virtual void initRenderData() {}

	// Called on initialization to create the default render pass.
	// The render pass will be used to optionally initialize rvg
	// and vui (see rvgSubpass) and in the default record implementation.
	// If neither are used, derived classes can return the empty render pass
	// here.
	virtual vpp::RenderPass createRenderPass();

	// Called to recreate all framebuffers (e.g. after resize).
	virtual void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>);

	// Default implementation of depth/multisample target creation.
	// Will be called by the default initBuffers implementation.
	virtual vpp::ViewableImage createDepthTarget(const vk::Extent2D&);
	virtual vpp::ViewableImage createMultisampleTarget(const vk::Extent2D&);

	virtual vpp::SwapchainPreferences swapchainPrefs() const;

	// Called for each buffer when rerecording.
	// By default will call beforeRender, start a render pass instance with
	// the created renderpass (see createRenderPass), call render, end
	// the render pass and call afterRender.
	virtual void record(const RenderBuffer&);

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
	virtual void samples(vk::SampleCountBits);

	// == Frame logic ==
	// Called every frame with the time since the last call to update
	// in seconds.
	virtual void update(double dt);

	// Called every frame when the last frame has finished and device
	// resources can therefore be accessed.
	virtual void updateDevice();

	// Schedules a rerecord. Will happen during the next updateDevice.
	void scheduleRerecord() { rerecord_ = true; }

	// Schedules a redraw. App will only redraw when this is called (or
	// the window send a draw event). Should be called from the update
	// method when it detects that new/changed content can be rendered.
	void scheduleRedraw() { redraw_ = true; }

	// Adds the given semaphore to the semaphores to wait for next frame
	// (in the given stage).
	void addSemaphore(vk::Semaphore, vk::PipelineStageFlags waitDst);
	void callUpdate();

	/// Utility function that opens an asset for reading.
	/// When on desktop, will try to open the file in the current directory
	/// or the asset/ source dir, while on android, it will use the asset
	/// manager.
	File openAsset(nytl::StringParam path, bool binary = true);

	// events
	virtual void resize(const ny::SizeEvent&);
	virtual bool key(const ny::KeyEvent&);
	virtual bool mouseButton(const ny::MouseButtonEvent&);
	virtual void mouseMove(const ny::MouseMoveEvent&);
	virtual bool mouseWheel(const ny::MouseWheelEvent&);
	virtual void mouseCross(const ny::MouseCrossEvent&);
	virtual void focus(const ny::FocusEvent&);
	virtual void close(const ny::CloseEvent&);
	virtual bool touchBegin(const ny::TouchBeginEvent&);
	virtual bool touchEnd(const ny::TouchEndEvent&);
	virtual void touchUpdate(const ny::TouchUpdateEvent&);
	virtual void touchCancel(const ny::TouchCancelEvent&) {}

protected:
	struct RenderImpl;
	struct Impl;
	std::unique_ptr<Impl> impl_;

	bool run_ {};
	bool resize_ {};
	bool rerecord_ {};
	bool redraw_ {};
	bool surface_ {};

	enum class DevType {
		igpu,
		dgpu,
		choose
	};

	struct {
		bool vsync = false;
		bool layers = true;
		bool renderdoc = false;
		unsigned samples = 1;
		std::variant<DevType, unsigned, const char*> phdev = DevType::choose;
	} args_ {};
};

} // namespace tkn
