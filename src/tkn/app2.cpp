#include <tkn/app2.hpp>
#include <dlg/dlg.hpp>
#include <nytl/functionTraits.hpp>
#include <argagg.hpp>
#include <iostream>
#include <thread>

namespace tkn {
inline namespace app2 {

namespace {

// We do this to allowe switching in normal if expressions
// instead of compile-time switches where possible
#ifdef __ANDROID__
	static bool onAndroid = true;
#else
	static bool onAndroid = false;
#endif

static unsigned dlgErrors = 0;
static unsigned dlgWarnings = 0;
static dlg_handler old_dlg_handler = NULL;
static void* old_dlg_data = NULL;

void dlgHandler(const struct dlg_origin* origin, const char* string, void*) {
	if(origin->level == dlg_level_error) {
		++dlgErrors;
	} else if(origin->level == dlg_level_warn) {
		++dlgWarnings;
	}

	old_dlg_handler(origin, string, old_dlg_data);
}

template<typename F, F f, typename Signature>
struct SwaMemberCallback;

template<typename F, F f, typename R, typename... Args>
struct SwaMemberCallback<F, f, R(Args...)> {
	static auto call(swa_window* win, Args... args) {
		App* app = static_cast<App*>(swa_window_get_userdata(win));
		return (app->*f)(std::forward<Args>(args)...);
	}
};

template<auto f> constexpr auto swaMemberCallback =
	&SwaMemberCallback<std::decay_t<decltype(f)>, f,
		typename nytl::FunctionTraits<decltype(f)>::Signature>::call;

} // anon namespace

struct SwaCallbacks {
	static void resize(struct swa_window* w, unsigned width, unsigned height) {
		((App*) swa_window_get_userdata(w))->resize(width, height);
	}

	static void key(struct swa_window* w, const swa_key_event* ev) {
		((App*) swa_window_get_userdata(w))->key(*ev);
	}

	static void mouseButton(struct swa_window* w, const swa_mouse_button_event* ev) {
		((App*) swa_window_get_userdata(w))->mouseButton(*ev);
	}

	static void mouseMove(struct swa_window* w, const swa_mouse_move_event* ev) {
		((App*) swa_window_get_userdata(w))->mouseMove(*ev);
	}

	static void mouseWheel(struct swa_window* w, float x, float y) {
		((App*) swa_window_get_userdata(w))->mouseWheel(x, y);
	}

	static void mouseCross(struct swa_window* w, const swa_mouse_cross_event* ev){
		((App*) swa_window_get_userdata(w))->mouseCross(*ev);
	}

	static void windowFocus(struct swa_window* w, bool gained) {
		((App*) swa_window_get_userdata(w))->windowFocus(gained);
	}

	static void windowDraw(struct swa_window* w) {
		((App*) swa_window_get_userdata(w))->windowDraw();
	}

	static void windowClose(struct swa_window* w) {
		((App*) swa_window_get_userdata(w))->windowClose();
	}

	static void windowState(struct swa_window* w, swa_window_state state) {
		((App*) swa_window_get_userdata(w))->windowState(state);
	}

	static void touchBegin(struct swa_window* w, const swa_touch_event* ev) {
		((App*) swa_window_get_userdata(w))->touchBegin(*ev);
	}

	static void touchEnd(struct swa_window* w, unsigned id) {
		((App*) swa_window_get_userdata(w))->touchEnd(id);
	}

	static void touchUpdate(struct swa_window* w, const swa_touch_event* ev) {
		((App*) swa_window_get_userdata(w))->touchUpdate(*ev);
	}

	static void touchCancel(struct swa_window* w, const swa_touch_event* ev) {
		((App*) swa_window_get_userdata(w))->touchCancel(*ev);
	}

	static void surfaceDestroyed(struct swa_window* w){
		((App*) swa_window_get_userdata(w))->surfaceDestroyed();
	}

	static void surfaceCreated(struct swa_window* w) {
		((App*) swa_window_get_userdata(w))->surfaceCreated();
	}
};

bool App::init(nytl::Span<const char*> args) {
	Args out;
	return init(args, out);
}

bool App::init(nytl::Span<const char*> args, Args& argsOut) {
	// We set a custom dlg handler that allows to set breakpoints
	// on any warnings or errors
	old_dlg_handler = dlg_get_handler(&old_dlg_data);
	dlg_set_handler(dlgHandler, nullptr);

	if(onAndroid) {
		// TODO: workaround atm, this seems to be needed since global
		// state often isn't reset when the app is reloaded. But we need
		// to reset it, otherwise we use obsolete function pointers.
		vk::dispatch = {};

		// TODO: we can't use layers on android by default.
		argsOut.layers = false;
	}

	if(!args.empty()) {
		auto parser = argParser();
		auto usage = std::string("Usage: ") + args[0] + " " + usageParams() + "\n\n";
		argagg::parser_results result;
		try {
			result = parser.parse(args.size(), args.data());
		} catch(const std::exception& error) {
			argagg::fmt_ostream help(std::cerr);
			help << usage << parser << "\n";
			help << "Invalid arguments: " << error.what();
			help << std::endl;
			return false;
		}

		if(result["help"]) {
			argagg::fmt_ostream help(std::cerr);
			help << usage << parser << std::endl;
			return false;
		}

		if(!handleArgs(result, argsOut)) {
			argagg::fmt_ostream help(std::cerr);
			help << usage << parser << std::endl;
			return false;
		}
	}

	dpy_.reset(swa_display_autocreate(name()));
	if(!dpy_) {
		dlg_fatal("Can't find backend for window creation (swa)");
		return false;
	}

	auto* dpy = dpy_.get();
	auto dpyCaps = swa_display_capabilities(dpy);
	if(!(dpyCaps & swa_display_cap_vk)) {
		dlg_fatal("Window backend does not support vulkan (swa)");
		return false;
	}

	// auto iniexts = vk::enumerateInstanceExtensionProperties(nullptr);
	// for(auto& ext : iniexts) {
	// 	dlg_trace("Instance extension: {}", ext.extensionName.data());
	// }

	std::vector<const char*> iniExts;
	unsigned swaExtsCount;
	auto swaExts = swa_display_vk_extensions(dpy, &swaExtsCount);
	iniExts.insert(iniExts.end(), swaExts, swaExts + swaExtsCount);
	iniExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	if(argsOut.layers) {
		iniExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}

	if(onAndroid) {
		iniExts.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
		// iniExtensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
	}

	// TODO: don't require 1.1 since it's not really needed for any app
	// (just some tests with it in sen). Also not supported in vkpp yet
	vk::ApplicationInfo appInfo(this->name(), 1, "tkn", 1, VK_API_VERSION_1_1);
	vk::InstanceCreateInfo instanceInfo;
	instanceInfo.pApplicationInfo = &appInfo;

	instanceInfo.enabledExtensionCount = iniExts.size();
	instanceInfo.ppEnabledExtensionNames = iniExts.data();

	std::vector<const char*> layers;
	if(argsOut.layers) {
		layers.push_back("VK_LAYER_KHRONOS_validation");
	}

	if(argsOut.renderdoc) {
		layers.push_back("VK_LAYER_RENDERDOC_Capture");
	}

	if(!layers.empty()) {
		instanceInfo.enabledLayerCount = layers.size();
		instanceInfo.ppEnabledLayerNames = layers.data();
	}

	try {
		instance_ = {instanceInfo};
		if(!instance_.vkInstance()) { // not supposed to happen
			dlg_fatal("vkCreateInstance returned a nullptr?!");
			return false;
		}
	} catch(const vk::VulkanError& error) {
		auto name = vk::name(error.error);
		dlg_error("Vulkan instance creation failed: {}", name);
		dlg_error("Your system may not support vulkan");
		dlg_error("This application requires vulkan to work");
		return false;
	}

	if(argsOut.layers) {
		messenger_.emplace(instance_);
	}

	// create window
	swa_window_settings wins;
	swa_window_settings_default(&wins);
	wins.surface = swa_surface_vk;
	wins.surface_settings.vk.instance = (uintptr_t) instance_.vkHandle();

	struct swa_window_listener wl {};
	wl.draw = swaMemberCallback<&App::windowDraw>;
	wl.close = swaMemberCallback<&App::windowClose>;
	wl.resize = swaMemberCallback<&App::resize>;
	wl.surface_destroyed = swaMemberCallback<&App::surfaceDestroyed>;
	wl.surface_created = swaMemberCallback<&App::surfaceCreated>;
	wl.key = swaMemberCallback<&App::key>;
	wl.state = swaMemberCallback<&App::windowState>;
	wl.focus = swaMemberCallback<&App::windowFocus>;
	wl.mouse_move = swaMemberCallback<&App::mouseMove>;
	wl.mouse_cross = swaMemberCallback<&App::mouseCross>;
	wl.mouse_button = swaMemberCallback<&App::mouseButton>;
	wl.mouse_wheel = swaMemberCallback<&App::mouseWheel>;
	wl.touch_begin = swaMemberCallback<&App::touchBegin>;
	wl.touch_end = swaMemberCallback<&App::touchEnd>;
	wl.touch_update = swaMemberCallback<&App::touchUpdate>;
	wl.touch_cancel = swaMemberCallback<&App::touchCancel>;
	wins.listener = &wl;

	win_.reset(swa_display_create_window(dpy, &wins));
	if(!win_) {
		dlg_fatal("Failed to create window (swa)");
		return false;
	}

	// create device
	// enable some extra features
	float priorities[1] = {0.0};

	auto phdevs = vk::enumeratePhysicalDevices(instance_);
	for(auto phdev : phdevs) {
		dlg_debug("Found device: {}", vpp::description(phdev, "\n\t"));
	}

	if(phdevs.empty()) {
		dlg_fatal("No physical devices (GPUs) with vulkan support present");
		return false;
	}

	auto vkSurf = (vk::SurfaceKHR) swa_window_get_vk_surface(win_.get());
	vk::PhysicalDevice phdev {};
	using std::get;
	if(argsOut.phdev.index() == 0 && get<0>(argsOut.phdev) == DevType::choose) {
		phdev = vpp::choose(phdevs, vkSurf);
	} else {
		auto i = argsOut.phdev.index();
		vk::PhysicalDeviceType type = {};
		if(i == 0) {
			type = get<0>(argsOut.phdev) == DevType::igpu ?
				vk::PhysicalDeviceType::integratedGpu :
				vk::PhysicalDeviceType::discreteGpu;
		}

		for(auto pd : phdevs) {
			auto p = vk::getPhysicalDeviceProperties(pd);
			if(i == 1 && p.deviceID == get<1>(argsOut.phdev)) {
				phdev = pd;
				break;
			} else if(i == 0 && p.deviceType == type) {
				phdev = pd;
				break;
			} else if(i == 2 && !std::strcmp(p.deviceName.data(),
					get<2>(argsOut.phdev))) {
				phdev = pd;
				break;
			}
		}
	}

	if(!phdev) {
		dlg_fatal("Could not find physical device");
		return false;
	}

	// auto devexts = vk::enumerateDeviceExtensionProperties(phdev, nullptr);
	// for(auto& ext : devexts) {
	// 	dlg_trace("Device extension: {}", ext.extensionName.data());
	// }

	auto p = vk::getPhysicalDeviceProperties(phdev);
	dlg_debug("Using device: {}", p.deviceName.data());

	auto queueFlags = vk::QueueBits::compute | vk::QueueBits::graphics;
	int queueFam = vpp::findQueueFamily(phdev, vkSurf, queueFlags);

	vk::DeviceCreateInfo devInfo;
	vk::DeviceQueueCreateInfo queueInfo({}, queueFam, 1, priorities);

	auto exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	devInfo.pQueueCreateInfos = &queueInfo;
	devInfo.queueCreateInfoCount = 1u;
	devInfo.ppEnabledExtensionNames = exts.begin();
	devInfo.enabledExtensionCount = 1u;
	devInfo.pEnabledFeatures = nullptr; // passed as pNext

	Features enable {}, f {};
	vk::getPhysicalDeviceFeatures2(phdev, f.base);
	if(!features(enable, f)) {
		dlg_fatal("Not all required features are supported by device");
		return false;
	}

	devInfo.pNext = &f.base;

	dev_.emplace(instance_, phdev, devInfo);
	presentq_ = vkDevice().queue(queueFam);

	// we query the surface information needed for swapchain creation
	// now since a lot of static resources (transitively) depend on it,
	// e.g. on the image format we use.
	// But we only create the swapchain (and will re-eval the needed size)
	// on the first resize event that provides us with the actual
	// init size of the window. That's why we pass a dummy size (1, 1) here.
	auto prefs = swapchainPrefs();
	swapchainInfo_ = vpp::swapchainCreateInfo(vkDevice(), vkSurf, {1, 1}, prefs);

	return true;
}

argagg::parser App::argParser() const {
	return {{
		{
			"help", {"-h", "--help"},
			"Displays help information", 0
		}, {
			"no-validation", {"--no-validation"},
			"Disabled layer validation", 0
		}, {
			"renderdoc", {"-r", "--renderdoc"},
			"Load the renderdoc vulkan layer", 0
		}, {
			"no-vsync", {"--no-vsync"},
			"Disable vsync", 0
		}, {
			"phdev", {"--phdev"},
			"Sets the physical device id to use."
			"Can be id, name or {igpu, dgpu, auto}", 1
		}
	}};
}

bool App::handleArgs(const argagg::parser_results& result, Args& args) {
	args.layers = !result["no-validation"];
	args.vsync = !result["no-vsync"];
	args.renderdoc = result["renderdoc"];

	auto& phdev = result["phdev"];
	if(phdev.count() > 0) {
		try {
			args.phdev = phdev.as<unsigned>();
		} catch(const std::exception&) {
			if(!std::strcmp(phdev[0].arg, "auto")) {
				args.phdev = DevType::choose;
			} else if(!std::strcmp(phdev[0].arg, "igpu")) {
				args.phdev = DevType::igpu;
			} else if(!std::strcmp(phdev[0].arg, "dgpu")) {
				args.phdev = DevType::dgpu;
			} else {
				args.phdev = phdev[0].arg;
			}
		}
	} else {
		args.phdev = DevType::choose;
	}

	return true;
}

bool App::features(Features& enable, const Features& supported) {
	(void) enable;
	(void) supported;
	return true;
}

// This may seem overly complicated but the main idea is this:
// either we receive a critical error/unexpected situation or we
// render a frame, even if we initially got outOfDate or the surface
// gets lost or whatever. We have to do this since we want/have to guarantee
// that there is exactly one render submission per call to updateDevice,
// mainly due to the semaphores
std::optional<std::uint64_t> App::submitFrame() {
	std::optional<std::uint64_t> submitID;
	while(true) {

		// when this sets submitID to a valid value, a commandbuffer
		// was submitted. Presenting might still have failed but we
		// have to treat this as a regular frame (will just be skipped
		// on output).
		auto res = renderer_.render(&submitID, {nextFrameWait_});
		if(submitID) {
			return submitID;
		}

		// we land here when acquiring an image failed (or returned
		// that its suboptimal).
		// first we check whether its an expected error (due to
		// resizing) and we want to rety or if it's something else.
		dlg_assert(res != vk::Result::success);
		if(res == vk::Result::errorSurfaceLostKHR) {
			if(!swa_display_dispatch(swaDisplay(), true)) {
				dlg_info("swa_display_dispatch returned false");
				run_ = false;
				return std::nullopt;
			}

			// This is a weird case: we got errorSurfaceLost but
			// no swa surface destroy event. No idea what could be
			// the issue.
			if(hasSurface()) {
				dlg_fatal("Got unrecoverable vulkan surfaceLost error");
				return std::nullopt;
			}

			// wait until we have a surface again
			while(!hasSurface()) {
				if(!swa_display_dispatch(swaDisplay(), true)) {
					dlg_info("swa_display_dispatch returned false");
					run_ = false;
					return std::nullopt;
				}
			}

			continue;
		}

		auto retry = (res == vk::Result::errorOutOfDateKHR) ||
			(res == vk::Result::suboptimalKHR);
		if(!retry) {
			// Unexpected and critical error. Has nothing to do
			// with asynchronous resizing/rendering
			dlg_fatal("render error: {}", vk::name(res));
			return std::nullopt;
		}

		// so we know that acquiring the image probably failed
		// due to an unhandled resize event. Poll for events
		dlg_debug("Skipping suboptimal/outOfDate frame");

		// we assume here that a return value of suboptimal/outOfDate
		// means that there was a resize and that means we _must_ get
		// a resize event at some point.
		// If this assumption is wrong, the application will block
		// from here until resized
		while(!resize_) {
			if(!swa_display_dispatch(swaDisplay(), true)) {
				dlg_info("swa_display_dispatch returned false");
				run_ = false;
				return std::nullopt;
			}
		}

		// Handle the received resize event
		// We will try rendering/acquiring again in the next
		// loop iteration.
		resize_ = false;
		dlg_info("resize on outOfDate: {}", winSize_);
		renderer_.recreate({winSize_.x, winSize_.y}, swapchainInfo_);
	}
}

void App::run() {
	// initial settings
	run_ = true;
	rerecord_ = true;
	redraw_ = true;
	lastUpdate_ = Clock::now();

	using Secd = std::chrono::duration<double, std::ratio<1, 1>>;
	auto dt = [this]{
		auto now = Clock::now();
		auto diff = now - lastUpdate_;
		auto dt = std::chrono::duration_cast<Secd>(diff).count();
		lastUpdate_ = now;
		return dt;
	};

	std::optional<std::uint64_t> submitID {};
	auto& submitter = vkDevice().queueSubmitter();

	// initial update
	update(dt());

	while(run_) {
		// - update device data -
		// the device will not using any of the resources we change here
		updateDevice();

		// we have to resize here and not directly when we receive the
		// even since we handles even during the logical update step
		// in which we must change rendering resources.
		if(resize_) {
			resize_ = false;
			renderer_.recreate({winSize_.x, winSize_.y}, swapchainInfo_);
		} else if(rerecord_) {
			renderer_.invalidate();
		}

		rerecord_ = false;

		// - submit and present -

		// - update phase -
		while(run_ && (!redraw_ || !hasSurface())) {
			// TODO: ideally, we could use waitEvents in update
			// an only wake up if we need to. But that would need
			// a (threaded!) waking up algorithm and also require all
			// components to signal this correctly (e.g. gui textfield blink)
			// which is (currently) not worth it/possible.
			auto idleRate = 144.f;
			std::this_thread::sleep_for(Secd(1 / idleRate));
			update(dt());
		}
	}
}

void App::addSemaphore(vk::Semaphore seph, vk::PipelineStageFlags waitDst) {
	nextFrameWait_.push_back({seph, waitDst});
}

void App::resize(unsigned width, unsigned height) {
	winSize_ = {width, height};
	resize_ = true;
}

bool App::key(const swa_key_event&) {
}
bool App::mouseButton(const swa_mouse_button_event&) {
}
void App::mouseMove(const swa_mouse_move_event&) {
}
bool App::mouseWheel(float x, float y) {
}
void App::mouseCross(const swa_mouse_cross_event&) {
}
void App::windowFocus(bool gained) {
}
void App::windowDraw() {
}
void App::windowClose() {
}
void App::windowState(swa_window_state state) {
}
bool App::touchBegin(const swa_touch_event&) {
}
bool App::touchEnd(unsigned id) {
}
void App::touchUpdate(const swa_touch_event&) {
}
void App::touchCancel(const swa_touch_event&) {
}
void App::surfaceDestroyed()  {
}
void App::surfaceCreated() {
}

// main
int appMain(App& app, int argc, const char** argv) {
	try {
		if(!app.init({argv, argv + argc})) {
			return EXIT_FAILURE;
		}
	} catch(const std::exception& err) {
		dlg_error("App initialization failed: {}", err.what());
		return EXIT_FAILURE;
	}

	app.run();
	return EXIT_SUCCESS;
}

}
} // namespace tkn
