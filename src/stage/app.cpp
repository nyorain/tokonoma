#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>

#include <rvg/context.hpp>
#include <rvg/state.hpp>
#include <rvg/font.hpp>
#include <vui/gui.hpp>
#include <vpp/device.hpp>
#include <vpp/physicalDevice.hpp>
#include <vpp/instance.hpp>
#include <vpp/debug.hpp>
#include <ny/appContext.hpp>
#include <ny/backend.hpp>
#include <ny/windowContext.hpp>
#include <ny/key.hpp>
#include <dlg/dlg.hpp>

#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <nytl/scope.hpp>

#include <argagg.hpp>
#include <optional>

namespace doi {

// constants
constexpr auto startMsaa = vk::SampleCountBits::e1;
constexpr auto clearColor = std::array<float, 4>{{0.f, 0.f, 0.f, 1.f}};
constexpr auto fontHeight = 11;
constexpr auto baseResPath = "../";

// util
namespace {

template<typename T>
void scale(nytl::Mat4<T>& mat, nytl::Vec3<T> fac) {
	for(auto i = 0; i < 3; ++i) {
		mat[i][i] *= fac[i];
	}
}

template<typename T>
void translate(nytl::Mat4<T>& mat, nytl::Vec3<T> move) {
	for(auto i = 0; i < 3; ++i) {
		mat[i][3] += move[i];
	}
}

} // anon namespace

// App::Impl
struct App::Impl {
	ny::Backend* backend;
	std::unique_ptr<ny::AppContext> ac;

	vpp::Instance instance;
	std::optional<vpp::Device> device;
	std::optional<vpp::DebugCallback> debugCallback;

	std::optional<MainWindow> window;
	std::optional<Renderer> renderer;

	std::optional<rvg::Context> rvgContext;
	rvg::Transform windowTransform;
	std::optional<rvg::FontAtlas> fontAtlas;
	std::optional<rvg::Font> defaultFont;
	std::optional<vui::Gui> gui;

	std::vector<vpp::StageSemaphore> nextFrameWait;
};

// App
App::App() = default;
App::~App() = default;

bool App::init(const AppSettings& settings) {
	impl_ = std::make_unique<Impl>();

	// arguments
	auto& args = settings.args;
	if(!args.empty()) {
		auto parser = argParser();
		auto usage = std::string("Usage: ") + args[0] + " [options]\n\n";
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

		handleArgs(result);
	}

	// init
	impl_->backend = &ny::Backend::choose();
	if(!impl_->backend->vulkan()) {
		throw std::runtime_error("ny backend has no vulkan support!");
	}

	impl_->ac = impl_->backend->createAppContext();

	// vulkan init: instance
	auto iniExtensions = appContext().vulkanExtensions();
	iniExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

	vk::ApplicationInfo appInfo(settings.name, 1, "doi", 1, VK_API_VERSION_1_0);
	vk::InstanceCreateInfo instanceInfo;
	instanceInfo.pApplicationInfo = &appInfo;

	instanceInfo.enabledExtensionCount = iniExtensions.size();
	instanceInfo.ppEnabledExtensionNames = iniExtensions.data();

	std::vector<const char*> layers;
	if(args_.layers) {
		layers.push_back("VK_LAYER_LUNARG_standard_validation");
	}

	if(args_.renderdoc) {
		layers.push_back("VK_LAYER_RENDERDOC_Capture");
	}

	if(!layers.empty()) {
		instanceInfo.enabledLayerCount = layers.size();
		instanceInfo.ppEnabledLayerNames = layers.data();
	}

	try {
		impl_->instance = {instanceInfo};
		if(!impl_->instance.vkInstance()) {
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

	// debug callback
	auto& ini = impl_->instance;
	if(args_.layers) {
		impl_->debugCallback.emplace(ini);
	}

	// init ny window
	impl_->window.emplace(*impl_->ac, ini);
	auto vkSurf = window().vkSurface();

	window().onResize = [&](const auto& ev) { resize(ev); };
	window().onKey = [&](const auto& ev) { key(ev); };
	window().onMouseMove = [&](const auto& ev) { mouseMove(ev); };
	window().onMouseButton = [&](const auto& ev) { mouseButton(ev); };
	window().onMouseWheel = [&](const auto& ev) { mouseWheel(ev); };
	window().onMouseCross = [&](const auto& ev) { mouseCross(ev); };
	window().onFocus = [&](const auto& ev) { focus(ev); };
	window().onClose = [&](const auto& ev) { close(ev); };

	// create device
	// enable some extra features
	float priorities[1] = {0.0};

	auto phdevs = vk::enumeratePhysicalDevices(ini);
	auto phdev = vpp::choose(phdevs, ini, vkSurf);

	auto queueFlags = vk::QueueBits::compute | vk::QueueBits::graphics;
	int queueFam = vpp::findQueueFamily(phdev, ini, vkSurf, queueFlags);

	vk::DeviceCreateInfo devInfo;
	vk::DeviceQueueCreateInfo queueInfo({}, queueFam, 1, priorities);

	auto exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	devInfo.pQueueCreateInfos = &queueInfo;
	devInfo.queueCreateInfoCount = 1u;
	devInfo.ppEnabledExtensionNames = exts.begin();
	devInfo.enabledExtensionCount = 1u;

	// TODO: enabled if supported
	auto features = vk::PhysicalDeviceFeatures {};
	features.shaderClipDistance = false;
	devInfo.pEnabledFeatures = &features;

	impl_->device.emplace(ini, phdev, devInfo);
	auto presentQueue = vulkanDevice().queue(queueFam);

	// renderer
	auto renderInfo = RendererCreateInfo {
		vulkanDevice(), vkSurf, window().size(), *presentQueue,
		startMsaa, args_.vsync, clearColor
	};

	impl_->renderer.emplace(renderInfo);
	renderer().beforeRender = [&](auto cb) {
		beforeRender(cb);
	};
	renderer().onRender = [&](auto cb) {
		render(cb);
	};
	renderer().afterRender = [&](auto cb) {
		afterRender(cb);
	};

	// additional stuff
	rvg::ContextSettings rvgcs {renderer().renderPass(), 0u};
	impl_->rvgContext.emplace(vulkanDevice(), rvgcs);
	impl_->windowTransform = {rvgContext()};
	impl_->fontAtlas.emplace(rvgContext());

	std::string fontPath = baseResPath;
	fontPath += "assets/Roboto-Regular.ttf";
	// fontPath += "assets/OpenSans-Light.ttf";
	// fontPath += "build/Lucida-Grande.ttf"; // nonfree
	impl_->defaultFont.emplace(*impl_->fontAtlas, fontPath, fontHeight);
	impl_->fontAtlas->bake(rvgContext());

	impl_->gui.emplace(rvgContext(), *impl_->defaultFont);

	return true;
}

argagg::parser App::argParser() const {
	return {{
		{
			"help",
			{"-h", "--help"},
			"Displays help information", 0
		}, {
			"no-validation",
			{"--no-validation"},
			"Disabled layer validation", 0
		}, {
			"renderdoc",
			{"-r", "--renderdoc"},
			"Load the renderdoc vulkan layer", 0
		}, {
			"no-vsync",
			{"--no-vsync"},
			"Disable vsync", 0
		}
	}};
}

void App::handleArgs(const argagg::parser_results& result) {
	args_.layers = !result["no-validation"];
	args_.vsync = !result["no-vsync"];
	args_.renderdoc = result["renderdoc"];
}

void App::render(vk::CommandBuffer) {
}
void App::beforeRender(vk::CommandBuffer) {
}
void App::afterRender(vk::CommandBuffer) {
}

void App::run() {
	run_ = true;
	rvgContext().rerecord(); // trigger initial record

	using Clock = std::chrono::high_resolution_clock;
	using Secf = std::chrono::duration<float, std::ratio<1, 1>>;
	auto lastFrame = Clock::now();

	std::uint64_t submitID {};

	// initial event poll
	if(!appContext().pollEvents()) {
		dlg_info("run initial pollEvents returned false");
		return;
	}

	while(run_) {
		// - update device data -
		if(updateDevice()) {
			renderer().invalidate();
		}

resize:
		if(resize_) {
			resize_ = {};
			renderer().resize(window().size());
		}

		// - submit and present -
		auto res = renderer().render(&submitID, {impl_->nextFrameWait});

		if(res != vk::Result::success) {
			auto retry = (res == vk::Result::errorOutOfDateKHR);
			if(retry) {
				dlg_debug("Skipping suboptimal/outOfDate frame");
				if(!appContext().pollEvents()) {
					dlg_info("upate pollEvents returned false");
					return;
				}

				// TODO: AAARGH
				goto resize;
			} else {
				dlg_error("render error: {}", vk::name(res));
				return;
			}
		} else {
			impl_->nextFrameWait.clear();
		}

		// wait for this frame at the end of this loop
		// this way we also wait for it when update throws
		// since we don't want to destroy resources before
		// rendering has finished
		auto waiter = nytl::ScopeGuard {[&] {
			vulkanDevice().queueSubmitter().wait(submitID);
		}};

		// - update logcial state -
		auto now = Clock::now();
		auto diff = now - lastFrame;
		auto dt = std::chrono::duration_cast<Secf>(diff).count();
		lastFrame = now;

		update(dt);
	}
}

void App::update(double dt) {
	if(!appContext().pollEvents()) {
		dlg_info("upate pollEvents returned false");
		run_ = false;
		return;
	}

	gui().update(dt);
}

bool App::updateDevice() {
	auto [rec, seph] = rvgContext().upload();
	if(seph) {
		impl_->nextFrameWait.push_back({seph,
			vk::PipelineStageBits::allGraphics});
	}

	rec |= gui().updateDevice();
	return rec;
}

void App::resize(const ny::SizeEvent& ev) {
	resize_ = true;

	// non-window but scaled normal cords
	// e.g. used for a level view
	/*
	auto w = ev.size.x / float(ev.size.y);
	auto h = 1.f;
	auto fac = 10 / std::sqrt(w * w + h * h);
	auto s = nytl::Vec { (2.f / (fac * w)), (-2.f / (fac * h)), 1 };
	*/

	// window-coords, origin bottom left
	auto s = nytl::Vec{ 2.f / ev.size.x, -2.f / ev.size.y, 1};
	auto transform = nytl::identity<4, float>();
	scale(transform, s);
	translate(transform, {-1, 1, 0});
	impl_->windowTransform.matrix(transform);

	// window-coords, origin top left
	s = nytl::Vec{ 2.f / ev.size.x, 2.f / ev.size.y, 1};
	transform = nytl::identity<4, float>();
	scale(transform, s);
	translate(transform, {-1, -1, 0});
	gui().transform(transform);
}

void App::close(const ny::CloseEvent&) {
	run_ = false;
}

void App::key(const ny::KeyEvent& ev) {
	gui().key({(vui::Key) ev.keycode, ev.pressed});
	if(ev.pressed && !ev.utf8.empty() && !ny::specialKey(ev.keycode)) {
		gui().textInput({ev.utf8.c_str()});
	}
}

void App::mouseButton(const ny::MouseButtonEvent& ev) {
	auto p = static_cast<nytl::Vec2f>(ev.position);
	auto b = static_cast<vui::MouseButton>(ev.button);
	gui().mouseButton({ev.pressed, b, p});
}

void App::mouseMove(const ny::MouseMoveEvent& ev) {
	gui().mouseMove({static_cast<nytl::Vec2f>(ev.position)});
}

void App::mouseWheel(const ny::MouseWheelEvent& ev) {
	gui().mouseWheel({ev.value.y});
}

void App::mouseCross(const ny::MouseCrossEvent& ev) {
	gui().mouseOver(ev.entered);
}

void App::focus(const ny::FocusEvent& ev) {
	gui().focus(ev.gained);
}

ny::AppContext& App::appContext() const {
	return *impl_->ac;
}

MainWindow& App::window() const {
	return *impl_->window;
}
Renderer& App::renderer() const {
	return *impl_->renderer;
}

vpp::Instance& App::vulkanInstance() const {
	return impl_->instance;
}
vpp::Device& App::vulkanDevice() const {
	return *impl_->device;
}

rvg::Context& App::rvgContext() const {
	return *impl_->rvgContext;
}

vui::Gui& App::gui() const {
	return *impl_->gui;
}

rvg::Transform& App::windowTransform() const {
	return impl_->windowTransform;
}

} // namespace doi
