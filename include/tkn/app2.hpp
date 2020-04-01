#pragma once

#include <swa/swa.h>
#include <vpp/vpp.hpp>
#include <rvg/context.hpp>
#include <vui/gui.hpp>
#include <nytl/span.hpp>

#include <memory>
#include <chrono>
#include <cstdint>
#include <optional>

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
	App() = default;
	App(App&&) = delete;
	App& operator=(App&&) = delete;

	virtual bool init(nytl::Span<const char*> args);
	virtual void run();

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

	// Called before device creation with the supported features
	// of the selected physical device (supported). Can be used to
	// enable the supported ones.
	virtual bool features(Features& enable, const Features& supported);

	// Returns the swapchain preferences based on which the swapchain
	// settings will be selected.
	virtual vpp::SwapchainPreferences swapchainPrefs() const;

	// Called for each buffer when rerecording.
	virtual void record(const RenderBuffer&) = 0;

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

	// Should return {} if rvg (and vui) are not needed, otherwise
	// the render and subpass they are used in.
	// NOTE: might be a limitation for some apps to only use rvg and vui
	// in one subpass, rvg requires it like that though. Changing it will
	// be hard since then rvg needs to compile multiple pipelines (one
	// for each render/subpass).
	virtual std::pair<vk::RenderPass, unsigned> rvgPass() const { return {}; }

	// Should be overwritten to return the name of this app.
	virtual const char* name() const { return "tkn"; }

	// Argument parsing
	virtual argagg::parser argParser() const;
	virtual bool handleArgs(const argagg::parser_results&, Args& out);
	virtual const char* usageParams() const { return "[options]"; }

	std::optional<std::uint64_t> submitFrame();

protected:
	swa_display* swaDisplay() const { return dpy_.get(); }
	swa_window* swaWindow() const { return win_.get(); }

	const vpp::Instance& vkInstance() const { return instance_; }
	vpp::Device& vkDevice() { return *dev_; }
	const vpp::Device& vkDevice() const { return *dev_; }
	const vpp::DebugMessenger& debugMessenger() const { return *messenger_; }

	rvg::Context& rvgContext() { return *rvg_; }
	vui::Gui& gui() { return *gui_; }
	nytl::Vec2ui windowSize() const { return winSize_; }
	bool hasSurface() const;

	// events
	friend struct SwaCallbacks;
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
	virtual void touchCancel(const swa_touch_event&);
	virtual void surfaceDestroyed();
	virtual void surfaceCreated();

protected:
	struct SwaDisplayDeleter {
		void operator()(swa_display* dpy) {
			swa_display_destroy(dpy);
		}
	};

	struct SwaWindowDeleter {
		void operator()(swa_window* win) {
			swa_window_destroy(win);
		}
	};

	class Renderer : public vpp::Renderer {
	public:
		Renderer() = default;

		void initBuffers(const vk::Extent2D&, nytl::Span<RenderBuffer>) override;
		void record(const RenderBuffer& buf) override;
		using vpp::Renderer::init;

		App* app_;
	};

	std::unique_ptr<swa_display, SwaDisplayDeleter> dpy_;
	vpp::Instance instance_;
	std::unique_ptr<swa_window, SwaWindowDeleter> win_;
	std::optional<vpp::DebugMessenger> messenger_;
	std::optional<vpp::Device> dev_;
	const vpp::Queue* presentq_;
	Renderer renderer_;

	std::vector<vpp::StageSemaphore> nextFrameWait_;
	vk::SwapchainCreateInfoKHR swapchainInfo_;
	std::optional<rvg::Context> rvg_;
	std::optional<vui::Gui> gui_;

	nytl::Vec2ui winSize_ {0, 0};

	using Clock = std::chrono::high_resolution_clock;
	Clock::time_point lastUpdate_;

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
};


int appMain(App& app, int argc, const char** argv);

template<typename AppT>
int appMain(int argc, const char** argv) {
	AppT app;
	return appMain(app, argc, argv);
}

}
} // namespace tkn
