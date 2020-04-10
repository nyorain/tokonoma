#pragma once

#include <swa/swa.h>
#include <vpp/fwd.hpp>
#include <rvg/fwd.hpp>
#include <vui/fwd.hpp>
#include <nytl/span.hpp>
#include <nytl/vec.hpp>

// - header shame -
// needed for features
// pulls in other vkpp headers. Maybe move features to extra header?
#include <vkpp/structs.hpp>
#include <vpp/renderer.hpp> // only needed for Renderer::RenderBuffer

#include <memory>
#include <variant>
#include <chrono>
#include <cstdint>
#include <optional>

struct dlg_origin;

namespace argagg { // fwd
	struct parser;
	struct parser_results;
} // namespace argagg

namespace tkn {
inline namespace app2 {

struct Features {
	vk::PhysicalDeviceFeatures2 base;
	vk::PhysicalDeviceMultiviewFeatures multiview;
	Features();
};

class App {
public:
	App();
	~App();

	App(App&&) = delete;
	App& operator=(App&&) = delete;

	virtual bool init(nytl::Span<const char*> args);
	virtual void run();

	swa_display* swaDisplay() const;
	swa_window* swaWindow() const;

	const vpp::Instance& vkInstance() const;
	vpp::Device& vkDevice();
	const vpp::Device& vkDevice() const;
	vpp::DebugMessenger& debugMessenger();
	const vpp::DebugMessenger& debugMessenger() const;
	const vk::SwapchainCreateInfoKHR& swapchainInfo() const;
	vk::SwapchainCreateInfoKHR& swapchainInfo();
	vpp::Renderer& renderer();
	const vpp::Renderer& renderer() const;

	rvg::Context& rvgContext();
	const rvg::Font& defaultFont() const;
	rvg::FontAtlas& fontAtlas();
	vui::Gui& gui();
	nytl::Vec2ui windowSize() const;
	bool hasSurface() const;

protected:
	using RenderBuffer = vpp::Renderer::RenderBuffer;

	enum class DevType {
		igpu,
		dgpu,
		choose
	};

	struct Args {
		bool vsync = false;
		bool layers = true;
		bool renderdoc = false;
		std::variant<DevType, unsigned, const char*> phdev = DevType::choose;
	};

	virtual bool init(nytl::Span<const char*> args, Args& out);

	// Called when the render buffers have to be initialized, e.g.
	// at the beginning or after an update.
	virtual void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) = 0;

	// Called for each buffer when rerecording.
	virtual void record(const RenderBuffer&) = 0;

	// Called when submitting a render buffer, see vpp::Renderer::submit.
	virtual vk::Semaphore submit(const RenderBuffer& buf,
		const vpp::RenderInfo& info, std::optional<std::uint64_t>* sid);

	// Called before device creation with the supported features
	// of the selected physical device (supported). Can be used to
	// enable the supported ones.
	virtual bool features(Features& enable, const Features& supported);

	// Returns the swapchain preferences based on which the swapchain
	// settings will be selected.
	virtual vpp::SwapchainPreferences swapchainPrefs(const Args&) const;

	// Called every frame with the time since the last call to update
	// in seconds. Must not change any device resources since rendering
	// might still be active. See updateDevice.
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

	// Initializes rvg and gui for use.
	// NOTE: might be a limitation for some apps to only use rvg and vui
	// in one subpass, rvg requires it like that though. Changing it will
	// be hard since then rvg needs to compile multiple pipelines (one
	// for each render/subpass).
	virtual void rvgInit(vk::RenderPass rp, unsigned subpass,
		vk::SampleCountBits samples);

	// Should be overwritten to return the name of this app.
	virtual const char* name() const { return "tkn"; }

	// Handler for dlg output (log & failed assertions).
	// The default implementation just calls the default handler.
	// But it also counts the number of error/warnings, mainly to
	// allow inserting breakpoints there.
	virtual void dlgHandler(const struct dlg_origin*, const char* string);

	// Handler for output from the vulkan debug messenger (vulkan layers).
	// The default implementation just calls the default handler.
	virtual void vkDebug(
		vk::DebugUtilsMessageSeverityBitsEXT,
		vk::DebugUtilsMessageTypeFlagsEXT,
		const vk::DebugUtilsMessengerCallbackDataEXT& data);

	// Argument parsing
	virtual argagg::parser argParser() const;
	virtual bool handleArgs(const argagg::parser_results&, Args& out);
	virtual const char* usageParams() const { return "[options]"; }

protected:
	// events
	virtual void resize(unsigned width, unsigned height);
	virtual bool key(const swa_key_event&);
	virtual bool mouseButton(const swa_mouse_button_event&);
	virtual void mouseMove(const swa_mouse_move_event&);
	virtual bool mouseWheel(float x, float y);
	virtual void mouseCross(const swa_mouse_cross_event&);
	virtual void windowFocus(bool gained);
	virtual void windowDraw();
	virtual void windowClose();
	virtual void windowState(swa_window_state state);
	virtual bool touchBegin(const swa_touch_event&);
	virtual bool touchEnd(unsigned id);
	virtual void touchUpdate(const swa_touch_event&);
	virtual void touchCancel();
	virtual void surfaceDestroyed();
	virtual void surfaceCreated();

	// stuff needed for own implementation
	std::optional<std::uint64_t> submitFrame();
	void dispatch();

private:
	struct Renderer;
	struct GuiListener;
	struct Callbacks;
	struct DebugMessenger;

	// We pImpl away certain app member for multiple reasons:
	// - allows us to keep App header complexity simple by only
	//   defining impl classes (such as Renderer or GuiListener)
	//   in the source
	// - allows us to not have to include headers that might not be
	//   needed by all apps, improving compile times
	struct Impl;
	std::unique_ptr<Impl> impl_;

	// Whether the app is supposed to run.
	// When this is set to false from within the main loop, the loop
	// will return.
	bool run_ {};

	// Whether a rerecord is needed. Will be done during the next
	// updateDevice phase.
	bool rerecord_ {};

	// Whether a redraw is needed. While this is false, won't draw
	// anything.
	bool redraw_ {};

	// Whether a resize event was received. Rendering might still be
	// active when we process events so we will recreate rendering
	// resources later on.
	bool resize_ {};
	nytl::Vec2ui winSize_ {0, 0};

	// available features
	bool hasClipDistance_ {}; // tracked for rvg
};


int appMain(App& app, int argc, const char** argv);

template<typename AppT>
int appMain(int argc, const char** argv) {
	AppT app;
	return appMain(app, argc, argv);
}

} // namespace app2
} // namespace tkn
