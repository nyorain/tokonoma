#include <stage/app.hpp>
#include <stage/window.hpp>

#include <rvg/context.hpp>
#include <rvg/state.hpp>
#include <rvg/font.hpp>
#include <vui/gui.hpp>
#include <vpp/device.hpp>
#include <vpp/physicalDevice.hpp>
#include <vpp/handles.hpp>
#include <vpp/formats.hpp>
#include <vpp/vk.hpp>
#include <vpp/submit.hpp>
#include <vpp/debug.hpp>
#include <ny/appContext.hpp>
#include <ny/asyncRequest.hpp>
#include <ny/backend.hpp>
#include <ny/cursor.hpp>
#include <ny/windowContext.hpp>
#include <ny/key.hpp>
#include <dlg/dlg.hpp>

#include <nytl/vecOps.hpp>
#include <nytl/matOps.hpp>
#include <nytl/scope.hpp>

#include <dlg/dlg.hpp>
#include <dlg/output.h>

#include <argagg.hpp>
#include <optional>
#include <thread>

namespace doi {

// constants
constexpr auto clearColor = std::array<float, 4>{{0.f, 0.f, 0.f, 1.f}};
constexpr auto fontHeight = 12;

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

// allow to set breakpoint for errors/warnings
static unsigned dlgErrors = 0;
static unsigned dlgWarnings = 0;
void dlgHandler(const struct dlg_origin* origin, const char* string, void* data) {
	if(origin->level == dlg_level_error) {
		++dlgErrors;
	} else if(origin->level == dlg_level_warn) {
		++dlgWarnings;
	}

	dlg_default_output(origin, string, data);
}

/// GuiListener
class TextDataSource : public ny::DataSource {
public:
	std::vector<ny::DataFormat> formats() const override {
		return {ny::DataFormat::text};
	}

	std::any data(const ny::DataFormat& format) const override {
		if(format != ny::DataFormat::text) {
			return {};
		}

		return {text};
	}

	std::string text;
};

class GuiListener : public vui::GuiListener {
public:
	void app(App& app) { app_ = &app; }
	ny::AppContext& ac() { return app_->appContext(); }
	ny::WindowContext& wc() { return app_->window().windowContext(); }

	void copy(std::string_view view) override {
		auto source = std::make_unique<TextDataSource>();
		source->text = view;
		ac().clipboard(std::move(source));
	}

	void cursor(vui::Cursor cursor) override {
		if(cursor == currentCursor_) {
			return;
		}

		currentCursor_ = cursor;
		auto c = static_cast<ny::CursorType>(cursor);
		wc().cursor({c});
	}

	bool pasteRequest(const vui::Widget& widget) override {
		auto& gui = widget.gui();
		auto offer = ac().clipboard();
		if(!offer) { // nothing in clipboard
			return false;
		}

		auto req = offer->data(ny::DataFormat::text);
		if(req->ready()) {
			std::any any = req.get();
			auto* pstr = std::any_cast<std::string>(&any);
			if(!pstr) {
				return false;
			}

			gui.paste(widget, *pstr);
			return true;
		}

		req->callback([this, &gui](auto& req){ dataHandler(gui, req); });
		reqs_.push_back({std::move(req), &widget});
		return true;
	}

	void dataHandler(vui::Gui& gui, ny::AsyncRequest<std::any>& req) {
		auto it = std::find_if(reqs_.begin(), reqs_.end(),
			[&](auto& r) { return r.request.get() == &req; });
		if(it == reqs_.end()) {
			dlg_error("dataHandler: invalid request");
			return;
		}

		std::any any = req.get();
		auto* pstr = std::any_cast<std::string>(&any);
		auto str = pstr ? *pstr : "";
		gui.paste(*it->widget, str);
		reqs_.erase(it);
	}

protected:
	struct Request {
		ny::DataOffer::DataRequest request;
		const vui::Widget* widget;
	};

	App* app_;
	std::vector<Request> reqs_;
	vui::Cursor currentCursor_ {};
};

} // anon namespace

Features::Features() {
	base.pNext = &multiview;
}

// RenderImpl
struct App::RenderImpl : public vpp::Renderer {
	App& app_;

	RenderImpl(App& app, const vpp::Queue& presentQueue,
		const vk::SwapchainCreateInfoKHR& sci) : app_(app) {
		// avoid initial record
		vpp::Renderer::init(sci, presentQueue, {}, RecordMode::onDemand);
		mode_ = RecordMode::all;
	}
	void record(const RenderBuffer& rb) override {
		app_.record(rb);
	}
	void initBuffers(const vk::Extent2D& extent,
			nytl::Span<RenderBuffer> bufs) override {
		app_.initBuffers(extent, bufs);
	}
	auto& buffers() {
		return renderBuffers_;
	}
};

using Clock = std::chrono::high_resolution_clock;
using Secf = std::chrono::duration<float, std::ratio<1, 1>>;

// App::Impl
struct App::Impl {
	ny::Backend* backend;
	std::unique_ptr<ny::AppContext> ac;

	vpp::Instance instance;
	std::optional<vpp::Device> device;
	std::optional<vpp::DebugMessenger> debugMessenger;

	std::optional<MainWindow> window;
	std::optional<RenderImpl> renderer;

	vpp::RenderPass renderPass;
	vpp::ViewableImage multisampleTarget;
	vpp::ViewableImage depthTarget;
	vk::Format depthFormat {vk::Format::undefined};
	vk::SwapchainCreateInfoKHR scInfo;

	// rvg
	std::optional<rvg::Context> rvgContext;
	rvg::Transform windowTransform;
	std::optional<rvg::FontAtlas> fontAtlas;
	std::optional<rvg::Font> defaultFont;
	GuiListener guiListener;
	std::optional<vui::Gui> gui;

	std::vector<vpp::StageSemaphore> nextFrameWait;
	vk::SampleCountBits samples;

	bool clipDistance {};
	Clock::time_point lastUpdate;
};

// App
App::App() = default;
App::~App() = default;

bool App::init(nytl::Span<const char*> args) {
	dlg_set_handler(dlgHandler, nullptr);
	impl_ = std::make_unique<Impl>();

	// arguments
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

		if(!handleArgs(result)) {
			argagg::fmt_ostream help(std::cerr);
			help << usage << parser << std::endl;
			return false;
		}
	}

	// init
	impl_->backend = &ny::Backend::choose();
	if(!impl_->backend->vulkan()) {
		throw std::runtime_error("ny backend has no vulkan support!");
	}

	impl_->ac = impl_->backend->createAppContext();

	// vulkan init: instance
	auto iniExtensions = appContext().vulkanExtensions();
	iniExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	// TODO: don't require 1.1 since it's not really needed for any app
	// (just some tests with it in sen). Also not supported in vkpp yet
	vk::ApplicationInfo appInfo(this->name(), 1, "doi", 1, VK_API_VERSION_1_1);
	vk::InstanceCreateInfo instanceInfo;
	instanceInfo.pApplicationInfo = &appInfo;

	instanceInfo.enabledExtensionCount = iniExtensions.size();
	instanceInfo.ppEnabledExtensionNames = iniExtensions.data();

	std::vector<const char*> layers;
	if(args_.layers) {
		layers.push_back("VK_LAYER_KHRONOS_validation");
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
		impl_->debugMessenger.emplace(ini);
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
	window().onDraw = [&](const auto&) { redraw_ = true; };

	// create device
	// enable some extra features
	float priorities[1] = {0.0};

	auto phdevs = vk::enumeratePhysicalDevices(ini);
	for(auto phdev : phdevs) {
		dlg_debug("Found device: {}", vpp::description(phdev, "\n\t"));
	}

	vk::PhysicalDevice phdev {};
	using std::get;
	if(args_.phdev.index() == 0 && get<0>(args_.phdev) == DevType::choose) {
		phdev = vpp::choose(phdevs, vkSurf);
	} else {
		auto i = args_.phdev.index();
		vk::PhysicalDeviceType type = {};
		if(i == 0) {
			type = get<0>(args_.phdev) == DevType::igpu ?
				vk::PhysicalDeviceType::integratedGpu :
				vk::PhysicalDeviceType::discreteGpu;
		}

		for(auto pd : phdevs) {
			auto p = vk::getPhysicalDeviceProperties(pd);
			if(i == 1 && p.deviceID == get<1>(args_.phdev)) {
				phdev = pd;
				break;
			} else if(i == 0 && p.deviceType == type) {
				phdev = pd;
				break;
			} else if(i == 2 && !std::strcmp(p.deviceName.data(), get<2>(args_.phdev))) {
				phdev = pd;
				break;
			}
		}
	}

	if(!phdev) {
		dlg_error("Could not find physical device");
		return false;
	}

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
		return false;
	}

	devInfo.pNext = &f.base;

	impl_->device.emplace(ini, phdev, devInfo);
	auto presentQueue = vulkanDevice().queue(queueFam);

	// renderer
	impl_->samples = vk::SampleCountBits::e1;
	switch(args_.samples) {
		case 1u: impl_->samples = vk::SampleCountBits::e1; break;
		case 2u: impl_->samples = vk::SampleCountBits::e2; break;
		case 4u: impl_->samples = vk::SampleCountBits::e4; break;
		case 8u: impl_->samples = vk::SampleCountBits::e8; break;
		case 16u: impl_->samples = vk::SampleCountBits::e16; break;
		default:
			dlg_fatal("Invalid multisample setting");
			return false;
	}

	vpp::SwapchainPreferences prefs {};
	if(args_.vsync) {
		prefs.presentMode = vk::PresentModeKHR::fifo; // vsync
	}
	auto size = window().size();
	impl_->scInfo = vpp::swapchainCreateInfo(vulkanDevice(), vkSurf,
		{size.x, size.y}, prefs);

	impl_->depthFormat = vk::Format::undefined;
	if(needsDepth()) {
		// find supported depth format
		vk::ImageCreateInfo img; // dummy for property checking
		img.extent = {1, 1, 1};
		img.mipLevels = 1;
		img.arrayLayers = 1;
		img.imageType = vk::ImageType::e2d;
		img.sharingMode = vk::SharingMode::exclusive;
		img.tiling = vk::ImageTiling::optimal;
		img.samples = samples();
		img.usage = vk::ImageUsageBits::depthStencilAttachment;
		img.initialLayout = vk::ImageLayout::undefined;

		auto fmts = {
			vk::Format::d32Sfloat,
			vk::Format::d32SfloatS8Uint,
			vk::Format::d24UnormS8Uint,
			vk::Format::d16Unorm,
			vk::Format::d16UnormS8Uint,
		};
		auto features = vk::FormatFeatureBits::depthStencilAttachment |
			vk::FormatFeatureBits::sampledImage;
		impl_->depthFormat = vpp::findSupported(vulkanDevice(), fmts,
			img, features);
		if(impl_->depthFormat == vk::Format::undefined) {
			throw std::runtime_error("No depth format supported");
		}
	}

	initRenderData();
	impl_->renderPass = createRenderPass();
	impl_->renderer.emplace(*this, *presentQueue, impl_->scInfo);

	// additional stuff
	auto [pass, subpass] = rvgPass();
	if(pass) {
		rvg::ContextSettings rvgcs {pass, subpass};
		rvgcs.samples = samples();
		rvgcs.clipDistanceEnable = impl_->clipDistance;

		impl_->rvgContext.emplace(vulkanDevice(), rvgcs);
		impl_->windowTransform = {rvgContext()};
		impl_->fontAtlas.emplace(rvgContext());

		std::string fontPath = DOI_BASE_DIR;
		fontPath += "/assets/Roboto-Regular.ttf";
		// fontPath += "assets/OpenSans-Light.ttf";
		// fontPath += "build/Lucida-Grande.ttf"; // nonfree
		impl_->defaultFont.emplace(*impl_->fontAtlas, fontPath);

		// gui
		impl_->guiListener.app(*this);
		impl_->gui.emplace(rvgContext(), *impl_->defaultFont, impl_->guiListener);
	}

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
			"multisamples", {"--multisamples", "-m"},
			"Sets the samples to use", 1
		}, {
			"phdev", {"--phdev"},
			"Sets the physical device id to use."
			"Can be id, name or {igpu, dgpu, auto}", 1
		}
	}};
}

bool App::handleArgs(const argagg::parser_results& result) {
	args_.layers = !result["no-validation"];
	args_.vsync = !result["no-vsync"];
	args_.renderdoc = result["renderdoc"];

	if(result.has_option("multisamples") > 0) {
		args_.samples = result["multisamples"].as<unsigned>(1);
	}

	auto& phdev = result["phdev"];
	if(phdev.count() > 0) {
		try {
			args_.phdev = phdev.as<unsigned>();
		} catch(const std::exception&) {
			if(!std::strcmp(phdev[0].arg, "auto")) {
				args_.phdev = DevType::choose;
			} else if(!std::strcmp(phdev[0].arg, "igpu")) {
				args_.phdev = DevType::igpu;
			} else if(!std::strcmp(phdev[0].arg, "dgpu")) {
				args_.phdev = DevType::dgpu;
			} else {
				args_.phdev = phdev[0].arg;
			}
		}
	} else {
		args_.phdev = DevType::choose;
	}

	return true;
}

bool App::features(Features& enable, const Features& supported) {
	if(supported.base.features.shaderClipDistance) {
		enable.base.features.shaderClipDistance = true;
		impl_->clipDistance = true;
	}

	return true;
}

void App::record(const RenderBuffer& buf) {
	const auto width = impl_->scInfo.imageExtent.width;
	const auto height = impl_->scInfo.imageExtent.height;

	auto cb = buf.commandBuffer;
	vk::beginCommandBuffer(cb, {});
	beforeRender(cb);

	auto cv = clearValues();
	vk::cmdBeginRenderPass(cb, {
		renderPass(),
		buf.framebuffer,
		{0u, 0u, width, height},
		std::uint32_t(cv.size()), cv.data()
	}, {});

	vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
	vk::cmdSetViewport(cb, 0, 1, vp);
	vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});

	render(cb);
	vk::cmdEndRenderPass(cb);
	afterRender(cb);
	vk::endCommandBuffer(cb);
}

vpp::ViewableImage App::createDepthTarget(const vk::Extent2D& size) {
	auto width = size.width;
	auto height = size.height;

	// img
	vk::ImageCreateInfo img;
	img.imageType = vk::ImageType::e2d;
	img.format = depthFormat();
	img.extent.width = width;
	img.extent.height = height;
	img.extent.depth = 1;
	img.mipLevels = 1;
	img.arrayLayers = 1;
	img.sharingMode = vk::SharingMode::exclusive;
	img.tiling = vk::ImageTiling::optimal;
	img.samples = samples();
	img.usage = vk::ImageUsageBits::depthStencilAttachment |
		vk::ImageUsageBits::sampled;
	img.initialLayout = vk::ImageLayout::undefined;

	// view
	vk::ImageViewCreateInfo view;
	view.viewType = vk::ImageViewType::e2d;
	view.format = img.format;
	view.components = {};
	view.subresourceRange.aspectMask = vk::ImageAspectBits::depth;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;

	// create the viewable image
	// will set the created image in the view info for us
	return {vulkanDevice().devMemAllocator(), img, view};
}

vpp::ViewableImage App::createMultisampleTarget(const vk::Extent2D& size) {
	auto width = size.width;
	auto height = size.height;

	// img
	vk::ImageCreateInfo img;
	img.imageType = vk::ImageType::e2d;
	img.format = impl_->scInfo.imageFormat;
	img.extent.width = width;
	img.extent.height = height;
	img.extent.depth = 1;
	img.mipLevels = 1;
	img.arrayLayers = 1;
	img.sharingMode = vk::SharingMode::exclusive;
	img.tiling = vk::ImageTiling::optimal;
	img.samples = samples();
	img.usage = vk::ImageUsageBits::transientAttachment | vk::ImageUsageBits::colorAttachment;
	img.initialLayout = vk::ImageLayout::undefined;

	// view
	vk::ImageViewCreateInfo view;
	view.viewType = vk::ImageViewType::e2d;
	view.format = img.format;
	view.components.r = vk::ComponentSwizzle::r;
	view.components.g = vk::ComponentSwizzle::g;
	view.components.b = vk::ComponentSwizzle::b;
	view.components.a = vk::ComponentSwizzle::a;
	view.subresourceRange.aspectMask = vk::ImageAspectBits::color;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;

	// create the viewable image
	// will set the created image in the view info for us
	return {vulkanDevice().devMemAllocator(), img, view};
}

std::vector<vk::ClearValue> App::clearValues() {
	std::vector<vk::ClearValue> clearValues;
	clearValues.reserve(3);
	vk::ClearValue c {{0.f, 0.f, 0.f, 0.f}};

	clearValues.push_back(c); // clearColor
	if(samples() != vk::SampleCountBits::e1) { // msaa attachment
		clearValues.push_back({c});
	}

	if(depthFormat() != vk::Format::undefined) {
		clearValues.emplace_back(c).depthStencil = {1.f, 0u};
	}

	return clearValues;
}

void App::initBuffers(const vk::Extent2D& size, nytl::Span<RenderBuffer> bufs) {
	std::vector<vk::ImageView> attachments {vk::ImageView {}};
	auto scPos = 0u; // attachments[scPos]: swapchain image

	if(samples() != vk::SampleCountBits::e1) {
		impl_->multisampleTarget = createMultisampleTarget(size);
		attachments.push_back(impl_->multisampleTarget.vkImageView());
	}

	if(depthFormat() != vk::Format::undefined) {
		impl_->depthTarget = createDepthTarget(size);
		attachments.push_back(impl_->depthTarget.vkImageView());
	}

	for(auto& buf : bufs) {
		attachments[scPos] = buf.imageView;
		vk::FramebufferCreateInfo info ({},
			renderPass(),
			attachments.size(),
			attachments.data(),
			size.width,
			size.height,
			1);
		buf.framebuffer = {vulkanDevice(), info};
	}
}

vpp::RenderPass App::createRenderPass() {
	vk::AttachmentDescription attachments[3] {};
	auto msaa = samples() != vk::SampleCountBits::e1;

	auto aid = 0u;
	auto depthid = 0u;
	auto resolveid = 0u;
	auto colorid = 0u;

	// swapchain color attachments
	// msaa: we resolve to this
	// otherwise this is directly rendered
	attachments[aid].format = swapchainInfo().imageFormat;
	attachments[aid].samples = vk::SampleCountBits::e1;
	attachments[aid].storeOp = vk::AttachmentStoreOp::store;
	attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
	attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
	attachments[aid].initialLayout = vk::ImageLayout::undefined;
	attachments[aid].finalLayout = vk::ImageLayout::presentSrcKHR;
	if(msaa) {
		attachments[aid].loadOp = vk::AttachmentLoadOp::dontCare;
		resolveid = aid;
	} else {
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		colorid = aid;
	}
	++aid;

	// optiontal multisampled render target
	if(msaa) {
		// multisample color attachment
		attachments[aid].format = swapchainInfo().imageFormat;
		attachments[aid].samples = samples();
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].storeOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::dontCare;
		attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].initialLayout = vk::ImageLayout::undefined;
		attachments[aid].finalLayout = vk::ImageLayout::presentSrcKHR;

		colorid = aid;
		++aid;
	}

	// optional depth target
	if(depthFormat() != vk::Format::undefined) {
		// depth attachment
		attachments[aid].format = depthFormat();
		attachments[aid].samples = samples();
		attachments[aid].loadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].storeOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].stencilLoadOp = vk::AttachmentLoadOp::clear;
		attachments[aid].stencilStoreOp = vk::AttachmentStoreOp::dontCare;
		attachments[aid].initialLayout = vk::ImageLayout::undefined;
		attachments[aid].finalLayout = vk::ImageLayout::depthStencilAttachmentOptimal;

		depthid = aid;
		++aid;
	}

	// refs
	vk::AttachmentReference colorReference;
	colorReference.attachment = colorid;
	colorReference.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference resolveReference;
	resolveReference.attachment = resolveid;
	resolveReference.layout = vk::ImageLayout::colorAttachmentOptimal;

	vk::AttachmentReference depthReference;
	depthReference.attachment = depthid;
	depthReference.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

	// deps
	std::vector<vk::SubpassDependency> dependencies;

	// TODO: do we really need this? isn't this detected by default?
	if(msaa) {
		dependencies.resize(2);

		dependencies[0].srcSubpass = vk::subpassExternal;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = vk::PipelineStageBits::bottomOfPipe;
		dependencies[0].dstStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dependencies[0].srcAccessMask = vk::AccessBits::memoryRead;
		dependencies[0].dstAccessMask = vk::AccessBits::colorAttachmentRead |
			vk::AccessBits::colorAttachmentWrite;
		dependencies[0].dependencyFlags = vk::DependencyBits::byRegion;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = vk::subpassExternal;
		dependencies[1].srcStageMask = vk::PipelineStageBits::colorAttachmentOutput;
		dependencies[1].dstStageMask = vk::PipelineStageBits::bottomOfPipe;
		dependencies[1].srcAccessMask = vk::AccessBits::colorAttachmentRead |
			vk::AccessBits::colorAttachmentWrite;
		dependencies[1].dstAccessMask = vk::AccessBits::memoryRead;
		dependencies[1].dependencyFlags = vk::DependencyBits::byRegion;
	}

	// only subpass
	vk::SubpassDescription subpass {};
	subpass.pipelineBindPoint = vk::PipelineBindPoint::graphics;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorReference;
	if(depthFormat() != vk::Format::undefined) {
		subpass.pDepthStencilAttachment = &depthReference;
	}

	if(samples() != vk::SampleCountBits::e1) {
		subpass.pResolveAttachments = &resolveReference;
	}

	// most general dependency
	// should cover almost all cases of external access to data that
	// is read during a render pass (host, transfer, compute shader)
	vk::SubpassDependency dependency;
	dependency.srcSubpass = vk::subpassExternal;
	dependency.srcStageMask =
		vk::PipelineStageBits::host |
		vk::PipelineStageBits::computeShader |
		vk::PipelineStageBits::colorAttachmentOutput |
		vk::PipelineStageBits::transfer;
	dependency.srcAccessMask = vk::AccessBits::hostWrite |
		vk::AccessBits::shaderWrite |
		vk::AccessBits::transferWrite |
		vk::AccessBits::colorAttachmentWrite;
	dependency.dstSubpass = 0u;
	dependency.dstStageMask = vk::PipelineStageBits::allGraphics;
	dependency.dstAccessMask = vk::AccessBits::uniformRead |
		vk::AccessBits::vertexAttributeRead |
		vk::AccessBits::indirectCommandRead |
		vk::AccessBits::shaderRead;
	dependencies.push_back(dependency);

	vk::RenderPassCreateInfo renderPassInfo;
	renderPassInfo.attachmentCount = aid;
	renderPassInfo.pAttachments = attachments;
	renderPassInfo.subpassCount = 1;
	renderPassInfo.pSubpasses = &subpass;
	renderPassInfo.dependencyCount = dependencies.size();
	renderPassInfo.pDependencies = dependencies.data();

	return {vulkanDevice(), renderPassInfo};
}

void App::run() {
	auto& renderer = *impl_->renderer;
	run_ = true;
	rerecord_ = true; // for initial recording
	redraw_ = true; // for initial drawing
	impl_->lastUpdate = Clock::now();

	std::optional<std::uint64_t> submitID {};
	auto& submitter = vulkanDevice().queueSubmitter();

	// initial event poll
	// if(!appContext().pollEvents()) {
	// 	dlg_info("run initial pollEvents returned false");
	// 	return;
	// }

	callUpdate();

	while(run_) {
		// - update device data -
		// the device will not using any of the resources we change here
		updateDevice();

		// we have to resize here and not directly when we receive the
		// even since we handles even during the logical update step
		// in which we must change rendering resources.
		if(resize_) {
			resize_ = {};
			auto size = window().size();
			renderer.recreate({size.x, size.y}, impl_->scInfo);
		} else if(rerecord_) {
			renderer.invalidate();
		}

		rerecord_ = false;

		// - submit and present -
		// we try to render and present (and if it does not succeed handle
		// pending resize event) so long until it works. This is needed since
		// we render and resize asynchronously<Paste>
		auto i = 0u;
		while(true) {

			// when this sets submitID to a valid value, a commandbuffer
			// was submitted. Presenting might still have failed but we
			// have to treat this as a regular frame (will just be skipped
			// on output).
			auto res = renderer.render(&submitID, {impl_->nextFrameWait});
			if(submitID) {
				break;
			}

			// we land here when acquiring an image failed (or returned
			// that its suboptimal).
			// first we check whether its an expected error (due to
			// resizing) and we want to rety or if it's something else.
			dlg_assert(res != vk::Result::success);
			auto retry = (res == vk::Result::errorOutOfDateKHR) ||
				(res == vk::Result::suboptimalKHR);
			if(!retry) {
				// Unexpected and critical error. Has nothing to do
				// with asynchronous resizing/rendering
				dlg_fatal("render error: {}", vk::name(res));
				return;
			}

			// so we know that acquiring the image probably failed
			// due to an unhandled resize event. Poll for events
			dlg_debug("Skipping suboptimal/outOfDate frame {}", ++i);

			// we assume here that a return value of suboptimal/outOfDate
			// means that there was a resize and that means we _must_ get
			// a resize event at some point.
			// If this assumption is wrong, the application will block
			// from here until resized
			while(!resize_) {
				if(!appContext().waitEvents()) {
					dlg_info("waitEvents returned false");
					return;
				}
			}

			// Handle the received resize event
			// We will try rendering/acquiring again in the next
			// loop iteration.
			resize_ = {};
			dlg_info("resize: {}", window().size());
			auto size = window().size();
			renderer.recreate({size.x, size.y}, impl_->scInfo);
		}

		impl_->nextFrameWait.clear();

		// wait for this frame at the end of this loop
		// this way we also wait for it when update throws
		// since we don't want to destroy resources before
		// rendering has finished
		auto waiter = nytl::ScopeGuard {[&] {
			submitter.wait(*submitID);
		}};

		// - update logcial state -
		// This must not touch device resources used for rendering in any way
		// since during this time the device is probably busy rendering,
		// it should be used to perform all heave host side computations
		redraw_ = false;
		callUpdate();

		// check if we can skip the next frame since nothing changed
		auto skipped = 0u;
		while(!redraw_ && !rerecord_ && run_) {
			// TODO: ideally, we could use waitEvents in update
			// an only wake up if we need to. But that would need
			// a (threaded!) waking up algorithm and also require all
			// components to signal this correctly (e.g. gui textfield blink)
			// which is (currently) not worth it/possible.
			auto idleRate = 60.f;
			std::this_thread::sleep_for(Secf(1 / idleRate));
			callUpdate();
			++skipped;
		}

		if(skipped > 0) {
			dlg_debug("Skipped {} idle frames", skipped);
		}
	}
}

void App::callUpdate() {
	auto now = Clock::now();
	auto diff = now - impl_->lastUpdate;
	auto dt = std::chrono::duration_cast<Secf>(diff).count();
	impl_->lastUpdate = now;
	update(dt);
}

void App::update(double dt) {
	if(!appContext().pollEvents()) {
		dlg_info("update: events returned false");
		run_ = false;
		return;
	}

	if(impl_->gui) {
		redraw_ |= gui().update(dt);
	}
}

void App::updateDevice() {
	if(!impl_->rvgContext) {
		return;
	}

	auto [rec, seph] = rvgContext().upload();
	if(seph) {
		impl_->nextFrameWait.push_back({seph,
			vk::PipelineStageBits::allGraphics});
	}

	rec |= gui().updateDevice();
	rerecord_ |= rec;
}

void App::addSemaphore(vk::Semaphore seph, vk::PipelineStageFlags waitDst) {
	impl_->nextFrameWait.push_back({seph, waitDst});
}

void App::resize(const ny::SizeEvent& ev) {
	resize_ = true;

	if(impl_->gui) {
		// TODO: probably best to remove this completetly
		// NOTE: currently top left (for mists)
		// window-coords, origin bottom left
		auto s = nytl::Vec{ 2.f / ev.size.x, 2.f / ev.size.y, 1};
		auto transform = nytl::identity<4, float>();
		scale(transform, s);
		translate(transform, {-1, -1, 0});
		impl_->windowTransform.matrix(transform);

		// window-coords, origin top left
		s = nytl::Vec{2.f / ev.size.x, 2.f / ev.size.y, 1};
		transform = nytl::identity<4, float>();
		scale(transform, s);
		translate(transform, {-1, -1, 0});
		gui().transform(transform);
	}
}

void App::samples(vk::SampleCountBits newSamples) {
	impl_->samples = newSamples;
	if(samples() != vk::SampleCountBits::e1) {
		createMultisampleTarget(swapchainInfo().imageExtent);
	}

	impl_->renderPass = createRenderPass();

	auto& renderer = *impl_->renderer;
	initBuffers(swapchainInfo().imageExtent, renderer.buffers());
	renderer.invalidate();
}

void App::close(const ny::CloseEvent&) {
	run_ = false;
}

bool App::key(const ny::KeyEvent& ev) {
	if(!impl_->gui) {
		return false;
	}

	auto ret = false;
	auto vev = vui::KeyEvent {};
	vev.key = static_cast<vui::Key>(ev.keycode); // both modeled after linux
	vev.modifiers = {static_cast<vui::KeyboardModifier>(ev.modifiers.value())};
	vev.pressed = ev.pressed;
	ret |= bool(gui().key(vev));

	auto textable = ev.pressed && !ev.utf8.empty();
	textable &= !ny::specialKey(ev.keycode);
	textable &= !(ev.modifiers & ny::KeyboardModifier::ctrl);
	if(textable) {
		ret |= bool(gui().textInput({ev.utf8.c_str()}));
	}

	return ret;
}

bool App::mouseButton(const ny::MouseButtonEvent& ev) {
	if(impl_->gui) {
		auto p = static_cast<nytl::Vec2f>(ev.position);
		auto b = static_cast<vui::MouseButton>(ev.button);
		return gui().mouseButton({ev.pressed, b, p});
	}

	return false;
}

void App::mouseMove(const ny::MouseMoveEvent& ev) {
	if(impl_->gui) {
		gui().mouseMove({static_cast<nytl::Vec2f>(ev.position)});
	}
}

bool App::mouseWheel(const ny::MouseWheelEvent& ev) {
	if(impl_->gui) {
		return gui().mouseWheel({ev.value, nytl::Vec2f(ev.position)});
	}

	return false;
}

void App::mouseCross(const ny::MouseCrossEvent& ev) {
	if(impl_->gui) {
		gui().mouseOver(ev.entered);
	}
}

void App::focus(const ny::FocusEvent& ev) {
	if(impl_->gui) {
		gui().focus(ev.gained);
	}
}

ny::AppContext& App::appContext() const {
	return *impl_->ac;
}

MainWindow& App::window() const {
	return *impl_->window;
}
vpp::Instance& App::vulkanInstance() const {
	return impl_->instance;
}
vpp::Device& App::vulkanDevice() const {
	return *impl_->device;
}

rvg::Context& App::rvgContext() const {
	dlg_assert(impl_->rvgContext);
	return *impl_->rvgContext;
}

vui::Gui& App::gui() const {
	dlg_assert(impl_->gui);
	return *impl_->gui;
}

rvg::Transform& App::windowTransform() const {
	return impl_->windowTransform;
}

vk::SampleCountBits App::samples() const {
	return impl_->samples;
}

vpp::RenderPass& App::renderPass() const {
	return impl_->renderPass;
}

vk::Format App::depthFormat() const {
	return impl_->depthFormat;
}

vpp::ViewableImage& App::depthTarget() const {
	return impl_->depthTarget;
}

vpp::ViewableImage& App::multisampleTarget() const {
	return impl_->multisampleTarget;
}

const vk::SwapchainCreateInfoKHR& App::swapchainInfo() const {
	return impl_->scInfo;
}

std::pair<vk::RenderPass, unsigned> App::rvgPass() const {
	return {renderPass(), 0};
}

rvg::Font& App::defaultFont() const {
	return *impl_->defaultFont;
}

vpp::DebugMessenger& App::debugMessenger() const {
	return *impl_->debugMessenger;
}

// free util
std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
		std::string_view glslPath) {
	static const auto spv = "live.frag.spv";
	std::string cmd = "glslangValidator -V -o ";
	cmd += spv;

	// include dirs
	cmd += " -I";
	cmd += DOI_BASE_DIR;
	cmd += "/src/shaders/include";

	// input
	auto fullPath = std::string(DOI_BASE_DIR);
	fullPath += "/src/";
	fullPath += glslPath;

	cmd += " ";
	cmd += fullPath;

	dlg_debug(cmd);

	// clearly mark glslang output
	struct dlg_style style {};
	style.style = dlg_text_style_bold;
	style.fg = dlg_color_magenta;
	style.bg = dlg_color_none;
	dlg_styled_fprintf(stderr, style, ">>> Start glslang <<<%s\n",
		dlg_reset_sequence);
	int ret = std::system(cmd.c_str());
	fflush(stdout);
	fflush(stderr);
	dlg_styled_fprintf(stderr, style, ">>> End glslang <<<%s\n",
		dlg_reset_sequence);

#ifdef DOI_LINUX
	if(WEXITSTATUS(ret) != 0) { // only working for posix
#else
	if(ret != 0) {
#endif
		dlg_error("Failed to compile shader {}", fullPath);
		return {};
	}

	return {{dev, spv}};
}

} // namespace doi
