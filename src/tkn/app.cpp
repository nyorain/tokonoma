#include <tkn/app.hpp>
#include <tkn/transform.hpp>
#include <tkn/features.hpp>
#include <tkn/shader.hpp>
#include <tkn/pipeline.hpp>
#include <tkn/threadPool.hpp>
#include <dlg/dlg.hpp>
#include <dlg/output.h>
#include <vpp/vk.hpp>
#include <vpp/renderer.hpp>
#include <vpp/debug.hpp>
#include <vpp/handles.hpp>
#include <vpp/device.hpp>
#include <vpp/submit.hpp>
#include <vpp/physicalDevice.hpp>
#include <nytl/scope.hpp>
#include <rvg/context.hpp>
#include <rvg/font.hpp>
#include <vui/gui.hpp>
#include <vui/style.hpp>
#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <argagg.hpp>
#include <iostream>
#include <thread>
#include <csignal>

// We do this to allowe switching in normal if expressions
// instead of compile-time switches where possible
#ifdef __ANDROID__
	constexpr bool onAndroid = true;
	constexpr auto fontHeight = 32;
#else
	constexpr bool onAndroid = false;
	constexpr auto fontHeight = 12;
#endif


namespace tkn {

Features::Features() {
	multiview.pNext = &descriptorIndexing;
	base.pNext = &multiview;
}

inline namespace app2 {

// App::Renderer
// We simply import the implementation from the App class basically.
// The reason we don't simply derive App from Renderer is that their
// interface is much cleaner this way (3 or 4 methods basically)
// and App *is not* really a Renderer, it just uses it.
struct App::Renderer : public vpp::Renderer {
	Renderer() = default;
	using vpp::Renderer::init;

	void initBuffers(const vk::Extent2D& size,
			nytl::Span<RenderBuffer> bufs) override {
		dlg_assert(app_);
		app_->initBuffers(size, bufs);
	}

	void record(const RenderBuffer& buf) override {
		dlg_assert(app_);
		app_->record(buf);
	}

	// Basically exports the protected method of vpp::Renderer
	// here as public method. Needed since we import all of this
	// functionality to the App class and want to allow App
	// to call the base implementation.
	vk::Semaphore baseSubmit(const RenderBuffer& buf,
			const vpp::RenderInfo& info,
			std::optional<std::uint64_t>* sid) {
		return vpp::Renderer::submit(buf, info, sid);
	}

	vk::Semaphore submit(const RenderBuffer& buf, const vpp::RenderInfo& info,
			std::optional<std::uint64_t>* sid) override {
		return app_->submit(buf, info, sid);
	}

	App* app_ {};
};

struct App::GuiListener : public vui::GuiListener {
	void cursor(vui::Cursor cursor) override {
		if(cursor == currentCursor_) {
			return;
		}

		currentCursor_ = cursor;
		auto num = static_cast<unsigned>(cursor) + 1;
		struct swa_cursor c {};
		c.type = static_cast<swa_cursor_type>(num);
		swa_window_set_cursor(app_->swaWindow(), c);
	}

	void focus([[maybe_unused]] vui::Widget* oldf,
			[[maybe_unused]] vui::Widget* nf) override {
#ifdef __ANDROID__
		// TODO: On android, show the keyboard when a textfield
		// (or anything text-y really) is focused.
#endif
	}

	App* app_ {};
	vui::Cursor currentCursor_ {};
};

struct App::Callbacks {
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

	static void touchCancel(struct swa_window* w) {
		((App*) swa_window_get_userdata(w))->touchCancel();
	}

	static void surfaceDestroyed(struct swa_window* w){
		((App*) swa_window_get_userdata(w))->surfaceDestroyed();
	}

	static void surfaceCreated(struct swa_window* w) {
		((App*) swa_window_get_userdata(w))->surfaceCreated();
	}

	static void dlgHandler(const struct dlg_origin* origin,
			const char* string, void* data) {
		auto app = static_cast<App*>(data);
		dlg_assert(app);
		if(app) {
			app->dlgHandler(origin, string);
		}
	}
};

struct App::DebugMessenger : public vpp::DebugMessenger {
	using vpp::DebugMessenger::DebugMessenger;
	App* app_;

	void call(MsgSeverity severity,
			MsgTypeFlags type, const Data& data) noexcept override {
		try {
			dlg_assert(app_);
			app_->vkDebug(severity, type, data);
		} catch(const std::exception& err) {
			dlg_error("Caught exception from vkDebug: {}", err.what());
		}
	}
};

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

struct App::Impl {
	// NOTE: order here is very important since a lot
	// of those depend on each other (in destructor)
	std::unique_ptr<swa_display, SwaDisplayDeleter> dpy;
	vpp::Instance instance;
	std::unique_ptr<swa_window, SwaWindowDeleter> win;
	std::optional<DebugMessenger> messenger;
	std::optional<vpp::Device> dev;
	const vpp::Queue* presentq {};

	std::vector<vpp::StageSemaphore> nextFrameWait;
	vk::SwapchainCreateInfoKHR swapchainInfo;
	Renderer renderer;

	std::optional<rvg::Context> rvg;
	std::optional<rvg::FontAtlas> fontAtlas;
	rvg::Font defaultFont;
	rvg::Transform windowTransform;
	std::optional<vui::Gui> gui;
	GuiListener guiListener;
	std::optional<vui::DefaultStyles> defaultStyles;

	struct {
		unsigned errors {};
		unsigned warnings {};
		dlg_handler oldHandler {};
		void* oldData {};
	} dlg;
};

App::App() = default;
App::~App() {
	// Make sure all jobs have finished.
	if(auto threadPoolPtr = ThreadPool::instanceIfExisting(); threadPoolPtr) {
		threadPoolPtr->destroy();
	}

#ifndef __ANDROID__
	if(impl_->dev) {
		// make sure all shader modules are destroyed before
		// the device is destroyed.
		tkn::ShaderCache::instance(*impl_->dev).clear();
		tkn::PipelineCache::finishInstance();
		tkn::ThreadState::finishInstance();
	}
#endif // __ANDROID__

	if(impl_ && impl_->dlg.oldHandler) {
		dlg_set_handler(impl_->dlg.oldHandler, impl_->dlg.oldData);
	}
}

bool App::init(nytl::Span<const char*> args) {
	Args out;
	return doInit(args, out);
}

bool App::doInit(nytl::Span<const char*> args, Args& argsOut) {
	impl_ = std::make_unique<Impl>();

	// We set a custom dlg handler that allows to set breakpoints
	// on any warnings or errors
	impl_->dlg.oldHandler = dlg_get_handler(&impl_->dlg.oldData);
	dlg_set_handler(&Callbacks::dlgHandler, this);

	if(onAndroid) {
		// TODO: workaround atm, this seems to be needed since global
		// state often isn't reset when the app is reloaded. But we need
		// to reset it, otherwise we use obsolete function pointers.
		vk::dispatch = {};

		// NOTE: we can't use layers on android by default.
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

		try {
			if(!handleArgs(result, argsOut)) {
				argagg::fmt_ostream help(std::cerr);
				help << usage << parser << std::endl;
				return false;
			}
		} catch(const std::exception& error) {
			argagg::fmt_ostream help(std::cerr);
			help << usage << parser << "\n";
			help << "Error prasing arguments: " << error.what();
			help << std::endl;
			return false;
		}
	}

	impl_->dpy.reset(swa_display_autocreate(name()));
	if(!impl_->dpy) {
		dlg_fatal("Can't find backend for window creation (swa)");
		return false;
	}

	auto* dpy = impl_->dpy.get();
	auto dpyCaps = swa_display_capabilities(dpy);
	if(!(dpyCaps & swa_display_cap_vk)) {
		dlg_fatal("Window backend does not support vulkan (swa)");
		return false;
	}

	std::vector<const char*> iniExts;
	auto iniexts = vk::enumerateInstanceExtensionProperties(nullptr);
	for(auto& ext : iniexts) {
		auto name = std::string_view(ext.extensionName.data());
		dlg_tracet(("vulkan-ini-exts"), "Instance extension: {}", name);

		if(name == VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) {
			dlg_info("Found ext_swapchain_color_space");
			iniExts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
		}

		// useful for swapchain color space
		if(name == VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) {
			iniExts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		}
	}

	unsigned swaExtsCount;
	auto swaExts = swa_display_vk_extensions(dpy, &swaExtsCount);
	iniExts.insert(iniExts.end(), swaExts, swaExts + swaExtsCount);

	if(argsOut.layers) {
		iniExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}

	if(onAndroid) {
		iniExts.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
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
		impl_->instance = {instanceInfo};
		if(!impl_->instance.vkInstance()) { // not supposed to happen
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
		impl_->messenger.emplace(vkInstance());
		impl_->messenger->app_ = this;
	}

	// create window
	swa_window_settings wins;
	swa_window_settings_default(&wins);
	wins.surface = swa_surface_vk;
	wins.surface_settings.vk.instance = (uintptr_t) vkInstance().vkHandle();

	// Wow, i really hate that C++ doesn't have designated initializers.
	// Using swa_window_listener really becomes a pain like that.
	static struct swa_window_listener wl {};
	wl.draw = &Callbacks::windowDraw;
	wl.close = &Callbacks::windowClose;
	wl.resize = &Callbacks::resize;
	wl.surface_destroyed = &Callbacks::surfaceDestroyed;
	wl.surface_created = &Callbacks::surfaceCreated;
	wl.key = &Callbacks::key;
	wl.state = &Callbacks::windowState;
	wl.focus = &Callbacks::windowFocus;
	wl.mouse_move = &Callbacks::mouseMove;
	wl.mouse_cross = &Callbacks::mouseCross;
	wl.mouse_button = &Callbacks::mouseButton;
	wl.mouse_wheel = &Callbacks::mouseWheel;
	wl.touch_begin = &Callbacks::touchBegin;
	wl.touch_end = &Callbacks::touchEnd;
	wl.touch_update = &Callbacks::touchUpdate;
	wl.touch_cancel = &Callbacks::touchCancel;
	wins.listener = &wl;

	impl_->win.reset(swa_display_create_window(dpy, &wins));
	if(!impl_->win) {
		dlg_fatal("Failed to create window (swa)");
		return false;
	}

	swa_window_set_userdata(swaWindow(), this);

	// create device
	// enable some extra features
	float priorities[1] = {0.0};

	auto phdevs = vk::enumeratePhysicalDevices(vkInstance());
	for(auto phdev : phdevs) {
		dlg_debug("Found device: {}", vpp::description(phdev, "\n\t"));
	}

	if(phdevs.empty()) {
		dlg_fatal("No physical devices (GPUs) with vulkan support present");
		return false;
	}

	auto vkSurf = (vk::SurfaceKHR) swa_window_get_vk_surface(swaWindow());
	if(!vkSurf) {
		dlg_fatal("swa window created without vk surface");
		return false;
	}

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

	auto devexts = vk::enumerateDeviceExtensionProperties(phdev, nullptr);
	std::vector<const char*> devExts = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	};

	for(auto& ext : devexts) {
		auto name = std::string_view(ext.extensionName.data());
		dlg_tracet(("vulkan-dev-exts"), "Device extension: {}", name);

		if(name == VK_EXT_HDR_METADATA_EXTENSION_NAME) {
			hasExtHdrMetadata_ = true;
			devExts.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
		}

		if(name == VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) {
			hasDescriptorIndexing_ = true;
			devExts.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
		}
	}

	auto p = vk::getPhysicalDeviceProperties(phdev);
	dlg_debug("Using device: {}", p.deviceName.data());

	auto queueFlags = vk::QueueBits::compute | vk::QueueBits::graphics;
	int queueFam = vpp::findQueueFamily(phdev, vkSurf, queueFlags);

	vk::DeviceCreateInfo devInfo;
	vk::DeviceQueueCreateInfo queueInfo({}, queueFam, 1, priorities);

	devInfo.pQueueCreateInfos = &queueInfo;
	devInfo.queueCreateInfoCount = 1u;
	devInfo.ppEnabledExtensionNames = devExts.data();
	devInfo.enabledExtensionCount = devExts.size();
	devInfo.pEnabledFeatures = nullptr; // passed as pNext

	Features enable {}, f {};
	vk::getPhysicalDeviceFeatures2(phdev, f.base);
	if(!features(enable, f)) {
		dlg_fatal("Not all required features are supported by device");
		return false;
	}

	devInfo.pNext = &f.base;

	impl_->dev.emplace(vkInstance(), phdev, devInfo);
	impl_->dev->hasDebugUtils = argsOut.layers;
	impl_->presentq = vkDevice().queue(queueFam);

	// we query the surface information needed for swapchain creation
	// now since a lot of static resources (transitively) depend on it,
	// e.g. on the image format we use.
	// But we only create the swapchain (and will re-eval the needed size)
	// on the first resize event that provides us with the actual
	// init size of the window. That's why we pass a dummy size (1, 1) here.
	auto prefs = swapchainPrefs(argsOut);
	impl_->swapchainInfo = vpp::swapchainCreateInfo(vkDevice(),
		vkSurf, {1, 1}, prefs);

	// TODO: remove
	// static vk::SurfaceFullScreenExclusiveWin32InfoEXT fwin;
	// fwin.hmonitor = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
//
	// static vk::SurfaceFullScreenExclusiveInfoEXT fullscreen;
	// fullscreen.fullScreenExclusive = vk::FullScreenExclusiveEXT::applicationControlled;

	// fullscreen.pNext = &fwin;
	// impl_->swapchainInfo.pNext = &fullscreen;

	if(hasExtColorSpace_) {
		// TODO: don't hardcode like this
		// TODO: communicate to application what colorspace it should output
		// TODO: hdr does only work in fullscreen on windows (amd, 2020).
		//   somehow account for that?
		auto formats = vk::getPhysicalDeviceSurfaceFormatsKHR(phdev, vkSurf);
		for(auto f : formats) {
			dlg_info("found swapchain format {} {}", (int) f.format, (int) f.colorSpace);
			if(f.format == vk::Format::r16g16b16a16Sfloat &&
					f.colorSpace == vk::ColorSpaceKHR::extendedSrgbLinearEXT) {
			// if(f.format == vk::Format::a2r10g10b10UnormPack32 &&
			// 		f.colorSpace == vk::ColorSpaceKHR::extendedSrgbNonlinearEXT) {
				impl_->swapchainInfo.imageFormat = f.format;
				impl_->swapchainInfo.imageColorSpace = f.colorSpace;
				dlg_info("  >> using");
				break;
			}
		}
	}

	return true;
}

void App::rvgInit(vk::RenderPass rp, unsigned subpass,
		vk::SampleCountBits samples) {

	rvg::ContextSettings rvgcs {rp, subpass};
	rvgcs.samples = samples;
	rvgcs.clipDistanceEnable = hasClipDistance_;

#ifdef __ANDROID__
	// TODO: needed because otherwise rvg uses too many bindings.
	// Should be fixed in rvg
	rvgcs.antiAliasing = false;
#endif // __ANDROID__

	impl_->rvg.emplace(vkDevice(), rvgcs);
	impl_->fontAtlas.emplace(rvgContext());

#ifdef __ANDROID__
	// TODO: android-specific swa stuff
	throw std::runtime_error("rvg/vui currently not supported on android");
	// auto& ac = dynamic_cast<ny::AndroidAppContext&>(appContext());
	// auto* mgr = ac.nativeActivity()->assetManager;
	// auto asset = AAssetManager_open(mgr, "font.ttf", AASSET_MODE_BUFFER);
	// dlg_assert(asset);
	// auto len = AAsset_getLength(asset);
	// auto buf = (std::byte*)AAsset_getBuffer(asset);
	// std::vector<std::byte> bytes(buf, buf + len);
	// impl_->defaultFont.emplace(*impl_->fontAtlas, std::move(bytes));
	// AAsset_close(asset);
#else
	std::string fontPath = TKN_BASE_DIR;
	fontPath += "/assets/Roboto-Regular.ttf";
	impl_->defaultFont = {*impl_->fontAtlas, fontPath};
#endif

	impl_->windowTransform = {rvgContext()};

	// gui
	impl_->guiListener.app_ = this;

	impl_->defaultStyles.emplace(rvgContext());
	auto& styles = impl_->defaultStyles->styles();
	styles.hint.font.height = fontHeight;
	styles.textfield.font.height = fontHeight;
	styles.labeledButton.font.height = fontHeight;

	impl_->gui.emplace(rvgContext(), impl_->defaultFont,
		std::move(styles), impl_->guiListener);
	impl_->gui->defaultFontHeight = fontHeight;
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
	args.phdev = DevType::choose;
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
	}

	return true;
}

bool App::features(Features& enable, const Features& supported) {
	if(supported.base.features.shaderClipDistance) {
		enable.base.features.shaderClipDistance = true;
		hasClipDistance_ = true;
	}

	return true;
}

vk::Semaphore App::submit(const RenderBuffer& buf,
		const vpp::RenderInfo& info, std::optional<std::uint64_t>* sid) {
	return impl_->renderer.baseSubmit(buf, info, sid);
}

// This may seem overly complicated but the main idea is this:
// either we receive a critical error/unexpected situation or we
// render a frame, even if we initially got outOfDate or the surface
// gets lost or whatever. We have to do this since we want/have to guarantee
// that there is exactly one render submission per call to updateDevice,
// mainly due to the semaphores of separate device work.
std::optional<std::uint64_t> App::submitFrame() {
	std::optional<std::uint64_t> submitID;
	while(true) {

		// when this sets submitID to a valid value, a commandbuffer
		// was submitted. Presenting might still have failed but we
		// have to treat this as a regular frame (will just be skipped
		// on output).
		auto res = impl_->renderer.render(&submitID, {impl_->nextFrameWait});
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
				dlg_fatal("Got surfaceLost error without surfaceDestroyed event");
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
		impl_->renderer.recreate({winSize_.x, winSize_.y}, swapchainInfo());
	}
}

void App::run() {
	// initial settings
	run_ = true;
	rerecord_ = true;
	redraw_ = true;

	using Clock = std::chrono::high_resolution_clock;
	using Secd = std::chrono::duration<double, std::ratio<1, 1>>;
	auto lastUpdate = Clock::now();
	auto lastUpdateDevice = Clock::now();

	// Calculate delta time since last update call
	auto dt = [](auto& lastTime){
		auto now = Clock::now();
		auto diff = now - lastTime;
		auto dt = std::chrono::duration_cast<Secd>(diff).count();
		lastTime = now;
		return dt;
	};

	std::optional<std::uint64_t> submitID {};
	auto& submitter = vkDevice().queueSubmitter();

	// initial update
	// Make sure we only start the main loop when we receive the
	// initial resize event to know our initial size.
	while(!resize_) {
		update(dt(lastUpdate));
	}

	// TODO: controlling whne vpp::Renderer rerecords is really unintuitive
	// atm (using recordMode). Try to fix that.
	while(run_) {
		// - update device data -
		// If we invalidate the command buffers (manualy via rerecord_
		// or via resize), make sure we only rerecord after updateDevice,
		// since that may recreate resources needed for recording.
		impl_->renderer.recordMode(vpp::Renderer::RecordMode::onDemand);

		// we have to resize here and not directly when we receive the
		// even since we handles even during the logical update step
		// in which we must change rendering resources.
		if(resize_) {
			// TODO: what if !hasSurface(). This is probably not handled
			// correctly here
			resize_ = false;
			if(!impl_->renderer.swapchain()) {
				impl_->renderer.app_ = this;
				vpp::updateImageExtent(vkDevice().vkPhysicalDevice(),
					impl_->swapchainInfo, {winSize_.x, winSize_.y});
				impl_->renderer.init(swapchainInfo(), *impl_->presentq,
					nullptr, vpp::Renderer::RecordMode::onDemand);
			} else {
				impl_->renderer.recreate({winSize_.x, winSize_.y}, swapchainInfo());
			}
		}

		// The device will not be using any of the resources we change here.
		// Make sure to call updateDevice *after* any new buffers have been
		// initialized (from resize).
		updateDevice(dt(lastUpdateDevice));
		if(rerecord_) {
			impl_->renderer.invalidate();
		}

		impl_->renderer.recordMode(vpp::Renderer::RecordMode::all);
		rerecord_ = false;


		// TODO: full hdr support
		// this is probably only useful for exclusive fullscreen apps?
		// scRGB
		// vk::HdrMetadataEXT hdr {};
		// hdr.displayPrimaryRed = {0.64, 0.33};
		// hdr.displayPrimaryGreen = {0.3, 0.6};
		// hdr.displayPrimaryBlue = {0.15, 0.06};
		// hdr.whitePoint = {0.3127, 0.3290};
		// hdr.maxLuminance = 0;
		// hdr.minLuminance = 400;
		// hdr.maxContentLightLevel = 0.f;
		// hdr.maxFrameAverageLightLevel = 400.f;
		// vk::setHdrMetadataEXT(vkDevice(), 1, impl_->renderer.swapchain(), hdr);

		// - submit and present -
		submitID = submitFrame();
		if(!submitID) {
			break;
		}

		impl_->nextFrameWait.clear();

		// wait for this frame at the end of this loop
		// this way we also wait for it when update throws
		// since we don't want to destroy resources before
		// rendering has finished
		auto waiter = nytl::ScopeGuard {[&] {
			submitter.wait(*submitID);
		}};

		// - update phase -
		redraw_ = false;
		update(dt(lastUpdate));

		while(run_ && !redraw_ && !resize_) {
			// TODO: ideally, we could use waitEvents in update
			// an only wake up if we need to. But that would need
			// a (threaded!) waking up algorithm and also require all
			// components to signal this correctly (e.g. gui textfield blink)
			// which is (currently) not worth it/possible.
			auto idleRate = 144.f;
			std::this_thread::sleep_for(Secd(1 / idleRate));
			update(dt(lastUpdate));
		}
	}
}

void App::dispatch() {
	if(!swa_display_dispatch(swaDisplay(), false)) {
		dlg_info("swa_display_dispatch returned false");
		run_ = false;
		return;
	}

	while(!hasSurface() || (!impl_->renderer.swapchain() && !resize_)) {
		if(!swa_display_dispatch(swaDisplay(), true)) {
			dlg_info("swa_display_dispatch returned false");
			run_ = false;
			break;
		}
	}
}

void App::update(double dt) {
	dispatch();

	if(impl_->gui) {
		redraw_ |= gui().update(dt);
	}
}

void App::updateDevice(double dt) {
	(void) dt;
	updateDevice();
}

void App::updateDevice() {
	if(impl_->rvg) {
		auto [rec, seph] = rvgContext().upload();
		if(seph) {
			impl_->nextFrameWait.push_back({seph,
				vk::PipelineStageBits::allGraphics});
		}

		if(impl_->gui) {
			rec |= gui().updateDevice();
		}

		rerecord_ |= rec;
	}
}

vpp::SwapchainPreferences App::swapchainPrefs(const Args& args) const {
	vpp::SwapchainPreferences prefs {};
	// prefs.format = vk::Format::r16g16b16a16Sfloat;
	// prefs.errorAction = vpp::SwapchainPreferences::ErrorAction::output;
	prefs.alpha = vk::CompositeAlphaBitsKHR::preMultiplied; // wayland
	if(args.vsync) {
		prefs.presentMode = vk::PresentModeKHR::fifo; // vsync
	}

	return prefs;
}

void App::addSemaphore(vk::Semaphore seph, vk::PipelineStageFlags waitDst) {
	impl_->nextFrameWait.push_back({seph, waitDst});
}

void App::resize(unsigned width, unsigned height) {
	dlg_debug("resize {} {}", width, height);

	// update gui transform if there is a gui
	if(impl_->gui) {
		auto s = nytl::Vec{2.f / width, 2.f / height, 1};
		auto transform = nytl::identity<4, float>();
		scale(transform, s);
		translate(transform, nytl::Vec3f {-1, -1, 0});
		gui().transform(transform);
	}

	// update window transform
	if(impl_->rvg) {
		auto s = nytl::Vec{ 2.f / width, 2.f / height, 1};
		auto transform = nytl::identity<4, float>();
		tkn::scale(transform, s);
		tkn::translate(transform, nytl::Vec3f {-1, -1, 0});
		impl_->windowTransform.matrix(transform);
	}

	winSize_ = {width, height};
	resize_ = true;
}

bool App::key(const swa_key_event& ev) {
	if(impl_->gui) {
		auto ret = false;
		auto vev = vui::KeyEvent {};
		vev.key = static_cast<vui::Key>(ev.keycode); // both modeled after linux
		vev.modifiers = {static_cast<vui::KeyboardModifier>(ev.modifiers)};
		vev.pressed = ev.pressed;
		ret |= bool(gui().key(vev));

		auto textable = ev.pressed && ev.utf8;
		textable &= swa_key_is_textual(ev.keycode);
		textable &= !(ev.modifiers & swa_keyboard_mod_ctrl);
		if(textable) {
			ret |= bool(gui().textInput({ev.utf8}));
		}

		return ret;
	}

	return false;
}

bool App::mouseButton(const swa_mouse_button_event& ev) {
	if(impl_->gui) {
		auto p = nytl::Vec2f{float(ev.x), float(ev.y)};
		auto num = static_cast<unsigned>(ev.button) + 1;
		auto b = static_cast<vui::MouseButton>(num);
		return gui().mouseButton({ev.pressed, b, p});
	}

	return false;
}
void App::mouseMove(const swa_mouse_move_event& ev) {
	if(impl_->gui) {
		gui().mouseMove({nytl::Vec2f{float(ev.x), float(ev.y)}});
	}
}
bool App::mouseWheel(float x, float y) {
	if(impl_->gui) {
		int mx, my;
		swa_display_mouse_position(swaDisplay(), &mx, &my);
		return gui().mouseWheel({{x, y}, {float(mx), float(my)}});
	}
	return false;
}
void App::mouseCross(const swa_mouse_cross_event& ev) {
	if(impl_->gui) {
		gui().mouseOver(ev.entered);
	}
}
void App::windowFocus(bool gained) {
	if(impl_->gui) {
		gui().focus(gained);
	}
}
void App::windowDraw() {
	// intentionally left blank
	// in future, we could optionally use smarter redraw scheduling
	// using this.
}
void App::windowClose() {
	dlg_debug("Window closed, exiting");
	run_ = false;
}
void App::windowState(swa_window_state state) {
	(void) state;
}
bool App::touchBegin(const swa_touch_event& ev) {
	if(impl_->gui) {
		return gui().mouseButton({true, vui::MouseButton::left,
			nytl::Vec2f{float(ev.x), float(ev.y)}});
	}
	return false;
}
bool App::touchEnd(unsigned id) {
	(void) id;
	if(impl_->gui) {
		// TODO: get last position?
		return gui().mouseButton({false, vui::MouseButton::left, {}});
	}
	return false;
}
void App::touchUpdate(const swa_touch_event& ev) {
	if(impl_->gui) {
		gui().mouseMove({nytl::Vec2f{float(ev.x), float(ev.y)}});
	}
}
void App::touchCancel() {
	// TODO: what does thi mean for gui?
}
void App::surfaceDestroyed()  {
	impl_->swapchainInfo.surface = {};
	impl_->renderer = {};
}
void App::surfaceCreated() {
	auto surf = swa_window_get_vk_surface(swaWindow());
	impl_->swapchainInfo.surface = (vk::SurfaceKHR) surf;
	impl_->renderer.init(swapchainInfo(), *impl_->presentq);
}
bool App::hasSurface() const {
	return swapchainInfo().surface;
}
swa_display* App::swaDisplay() const {
	return impl_->dpy.get();
}
swa_window* App::swaWindow() const {
	return impl_->win.get();
}
const vpp::Instance& App::vkInstance() const {
	return impl_->instance;
}
vpp::Device& App::vkDevice() {
	return *impl_->dev;
}
const vpp::Device& App::vkDevice() const {
	return *impl_->dev;
}
vpp::DebugMessenger& App::debugMessenger() {
	dlg_assert(impl_->messenger);
	return *impl_->messenger;
}
const vpp::DebugMessenger& App::debugMessenger() const {
	dlg_assert(impl_->messenger);
	return *impl_->messenger;
}
const vk::SwapchainCreateInfoKHR& App::swapchainInfo() const {
	return impl_->swapchainInfo;
}
vk::SwapchainCreateInfoKHR& App::swapchainInfo() {
	return impl_->swapchainInfo;
}
rvg::Context& App::rvgContext() {
	dlg_assertm(impl_->rvg, "rvg was not initialized");
	return *impl_->rvg;
}
const rvg::Transform& App::rvgWindowTransform() const {
	dlg_assertm(impl_->rvg, "rvg was not initialized");
	return impl_->windowTransform;
}
vui::Gui& App::gui() {
	dlg_assertm(impl_->gui, "rvg/vui was not initialized");
	return *impl_->gui;
}
nytl::Vec2ui App::windowSize() const {
	return winSize_;
}
const rvg::Font& App::defaultFont() const {
	return impl_->defaultFont;
}
rvg::FontAtlas& App::fontAtlas() {
	dlg_assertm(impl_->fontAtlas, "rvg was not initialized");
	return *impl_->fontAtlas;
}
vpp::Renderer& App::renderer() {
	return impl_->renderer;
}
const vpp::Renderer& App::renderer() const {
	return impl_->renderer;
}

void App::dlgHandler(const struct dlg_origin* origin, const char* string) {
	if(origin->level == dlg_level_error) {
		++impl_->dlg.errors;
	} else if(origin->level == dlg_level_warn) {
		++impl_->dlg.warnings;
	}

	// check for DLG_DEFAULT_TAGS
	for(auto it = origin->tags; *it; ++it) {
		constexpr struct {
			const char* name;
			dlg_level level; // everything below that is ignored
		} ignored[] = {
			{"tkn/pipeline", dlg_level_warn},
			{"tkn/shader", dlg_level_warn},
		};

		for(auto& ignore : ignored) {
			if(!std::strcmp(*it, ignore.name) && origin->level <= ignore.level) {
				return;
			}
		}
	}

	impl_->dlg.oldHandler(origin, string, impl_->dlg.oldData);

	if(origin->level == dlg_level_error) {
		std::raise(SIGINT); // tmp
	}
}

void App::vkDebug(
		vk::DebugUtilsMessageSeverityBitsEXT severity,
		vk::DebugUtilsMessageTypeFlagsEXT type,
		const vk::DebugUtilsMessengerCallbackDataEXT& data) {
	dlg_assert(impl_ && impl_->messenger);
	impl_->messenger->vpp::DebugMessenger::call(severity, type, data);
}

// TODO: make this a free function (independent from App) that
// just uses the static swa android instance. Not exposed publicly atm though.
File App::openAsset(nytl::StringParam path, bool binary) {
#ifdef __ANDROID__
	dlg_error("not implemented");
	return {};
	// auto& ac = dynamic_cast<ny::AndroidAppContext&>(appContext());
	// auto* mgr = ac.nativeActivity()->assetManager;
	// auto asset = AAssetManager_open(mgr, path.c_str(), 0);
	// return File(::funopen(asset, android::read, android::write,
	// 	android::seek, android::close));
#else
	auto mode = binary ? "rb" : "r";
	if(auto f = std::fopen(path.c_str(), mode); f) {
		return File(f);
	}

	auto path2 = std::string(TKN_BASE_DIR "/assets/") + path.c_str();
	return File(path2, mode);
#endif
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

