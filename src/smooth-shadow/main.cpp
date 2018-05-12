// Copyright (c) 2017 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

#include <stage/window.hpp>
#include <stage/render.hpp>
#include <argagg.hpp>

#include <vpp/instance.hpp>
#include <vpp/device.hpp>
#include <vpp/physicalDevice.hpp>
#include <vpp/debug.hpp>
#include <vpp/vk.hpp>

#include <ny/backend.hpp>
#include <ny/appContext.hpp>

#include <dlg/dlg.hpp>

constexpr auto appName = "smooth-shadow";
constexpr auto engineName = "vpp";
constexpr auto startMsaa = vk::SampleCountBits::e1;
constexpr auto clearColor = std::array<float, 4>{{0.f, 0.f, 0.f, 1.f}};

int main(int argc, const char** argv) {

	// - arguments -
	argagg::parser argParser {{
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

	auto usage = std::string("Usage: ") + argv[0] + " [options]\n\n";
	argagg::parser_results args;
	try {
		args = argParser.parse(argc, argv);
	} catch(const std::exception& error) {
		argagg::fmt_ostream help(std::cerr);
		help << usage << argParser << "\n";
		help << "Invalid arguments: " << error.what();
		help << std::endl;
		return EXIT_FAILURE;
	}

	if(args["help"]) {
		argagg::fmt_ostream help(std::cerr);
		help << usage << argParser << std::endl;
		return EXIT_SUCCESS;
	}

	auto useValidation = !args["no-validation"];
	auto vsync = !args["no-vsync"];

	// - initialization -
	auto& backend = ny::Backend::choose();
	if(!backend.vulkan()) {
		throw std::runtime_error("ny backend has no vulkan support!");
	}

	auto appContext = backend.createAppContext();

	// vulkan init: instance
	auto iniExtensions = appContext->vulkanExtensions();
	iniExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);

	vk::ApplicationInfo appInfo (appName, 1, engineName, 1, VK_API_VERSION_1_0);
	vk::InstanceCreateInfo instanceInfo;
	instanceInfo.pApplicationInfo = &appInfo;

	instanceInfo.enabledExtensionCount = iniExtensions.size();
	instanceInfo.ppEnabledExtensionNames = iniExtensions.data();

	std::vector<const char*> layers;
	if(useValidation) {
		layers.push_back("VK_LAYER_LUNARG_standard_validation");
	}

	if(args["renderdoc"]) {
		layers.push_back("VK_LAYER_RENDERDOC_Capture");
	}

	if(!layers.empty()) {
		instanceInfo.enabledLayerCount = layers.size();
		instanceInfo.ppEnabledLayerNames = layers.data();
	}

	vpp::Instance instance {};
	try {
		instance = {instanceInfo};
		if(!instance.vkInstance()) {
			throw std::runtime_error("vkCreateInstance returned a nullptr");
		}
	} catch(const vk::VulkanError& error) {
		auto name = vk::name(error.error);
		dlg_error("Vulkan instance creation failed: {}", name);
		dlg_error("Your system may not support vulkan");
		dlg_error("This application requires vulkan to work");
		return EXIT_FAILURE;
	}

	// debug callback
	std::unique_ptr<vpp::DebugCallback> debugCallback;
	if(useValidation) {
		debugCallback = std::make_unique<vpp::DebugCallback>(instance);
	}

	// init ny window
	MainWindow window(*appContext, instance);
	auto vkSurf = window.vkSurface();

	// create device
	// enable some extra features
	float priorities[1] = {0.0};

	auto phdevs = vk::enumeratePhysicalDevices(instance);
	auto phdev = vpp::choose(phdevs, instance, vkSurf);

	auto queueFlags = vk::QueueBits::compute | vk::QueueBits::graphics;
	int queueFam = vpp::findQueueFamily(phdev, instance, vkSurf, queueFlags);

	vk::DeviceCreateInfo devInfo;
	vk::DeviceQueueCreateInfo queueInfo({}, queueFam, 1, priorities);

	auto exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	devInfo.pQueueCreateInfos = &queueInfo;
	devInfo.queueCreateInfoCount = 1u;
	devInfo.ppEnabledExtensionNames = exts.begin();
	devInfo.enabledExtensionCount = 1u;

	auto features = vk::PhysicalDeviceFeatures {};
	features.shaderClipDistance = true;
	devInfo.pEnabledFeatures = &features;

	auto device = vpp::Device(instance, phdev, devInfo);
	auto presentQueue = device.queue(queueFam);

	auto renderInfo = RendererCreateInfo {
		device, vkSurf, window.size(), *presentQueue,
		startMsaa, vsync, clearColor
	};

	auto renderer = Renderer(renderInfo);

	// main loop stuff
	auto run = true;
	window.onResize = [&](const auto& ev) { renderer.resize(ev.size); };
	window.onClose = [&](const auto&) { run = false; };

	while(run) {
		if(!appContext->pollEvents()) {
			dlg_info("pollEvents returned false");
			return 0;
		}

		renderer.renderBlock();
	}
}
