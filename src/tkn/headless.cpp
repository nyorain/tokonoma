#include <tkn/headless.hpp>
#include <tkn/features.hpp>
#include <vpp/vk.hpp>
#include <vpp/physicalDevice.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

Headless::Headless(const HeadlessArgs& args) {
	auto iniExtensions = args.iniExts;
	iniExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	vk::ApplicationInfo appInfo("headless", 1, "tkn", 1, VK_API_VERSION_1_1);
	vk::InstanceCreateInfo instanceInfo;
	instanceInfo.pApplicationInfo = &appInfo;

	instanceInfo.enabledExtensionCount = iniExtensions.size();
	instanceInfo.ppEnabledExtensionNames = iniExtensions.data();

	std::vector<const char*> layers;
	if(args.layers) {
		layers.push_back("VK_LAYER_KHRONOS_validation");
	}

	if(args.renderdoc) {
		layers.push_back("VK_LAYER_RENDERDOC_Capture");
	}

	if(!layers.empty()) {
		instanceInfo.enabledLayerCount = layers.size();
		instanceInfo.ppEnabledLayerNames = layers.data();
	}

	try {
		instance = {instanceInfo};
		if(!instance.vkInstance()) {
			dlg_fatal("vkCreateInstance returned a nullptr?!");
			throw std::runtime_error("vkCreateInstance returned a nullptr?!");
		}
	} catch(const vk::VulkanError& error) {
		auto name = vk::name(error.error);
		dlg_error("Vulkan instance creation failed: {}", name);
		dlg_error("Your system may not support vulkan");
		dlg_error("This application requires vulkan to work");
		throw;
	}

	// debug messegner
	if(args.layers) {
		messenger = std::make_unique<vpp::DebugMessenger>(instance);
	}

	// create device
	// enable some extra features
	float priorities[1] = {0.0};

	auto phdevs = vk::enumeratePhysicalDevices(instance);
	for(auto phdev : phdevs) {
		dlg_debug("Found device: {}", vpp::description(phdev, "\n\t"));
	}

	vk::PhysicalDevice phdev {};
	using std::get;
	if(args.phdev.index() == 0 && get<0>(args.phdev) == DevType::choose) {
		phdev = vpp::choose(phdevs);
	} else {
		auto i = args.phdev.index();
		vk::PhysicalDeviceType type;
		if(i == 0) {
			type = get<0>(args.phdev) == DevType::igpu ?
				vk::PhysicalDeviceType::integratedGpu :
				vk::PhysicalDeviceType::discreteGpu;
		}

		for(auto pd : phdevs) {
			auto p = vk::getPhysicalDeviceProperties(pd);
			if(i == 1 && p.deviceID == get<1>(args.phdev)) {
				phdev = pd;
				break;
			} else if(i == 0 && p.deviceType == type) {
				phdev = pd;
				break;
			} else if(i == 2 && p.deviceName.data() == get<2>(args.phdev)) {
				phdev = pd;
				break;
			}
		}
	}

	// TODO: fall back to just another device here?
	if(!phdev) {
		dlg_error("Could not find physical device");
		throw std::runtime_error("Could not find physical device");
	}

	auto p = vk::getPhysicalDeviceProperties(phdev);
	dlg_debug("Using device: {}", p.deviceName.data());

	auto queueFlags = vk::QueueBits::compute | vk::QueueBits::graphics;
	int queueFam = vpp::findQueueFamily(phdev, queueFlags);

	vk::DeviceCreateInfo devInfo;
	vk::DeviceQueueCreateInfo queueInfo({}, queueFam, 1, priorities);

	Features enable {}, f {};
	if(args.featureChecker) {
		vk::getPhysicalDeviceFeatures2(phdev, f.base);
		if(!args.featureChecker(enable, f)) {
			throw std::runtime_error("Required feature not supported");
		}
		devInfo.pNext = &f.base;
	}

	devInfo.pQueueCreateInfos = &queueInfo;
	devInfo.queueCreateInfoCount = 1u;
	devInfo.ppEnabledExtensionNames = args.devExts.data();
	devInfo.enabledExtensionCount = args.devExts.size();

	// TODO: support letting the caller select features somehow?
	// auto f = vk::PhysicalDeviceFeatures {};
	// vk::getPhysicalDeviceFeatures(phdev, f);
	// vk::PhysicalDeviceFeatures enable;
	// if(!features(enable, f)) {
	// 	return false;
	// }
	// devInfo.pEnabledFeatures = &enable;

	device = std::make_unique<vpp::Device>(instance, phdev, devInfo);
}

// Args
argagg::parser HeadlessArgs::defaultParser() {
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
			"phdev", {"--phdev"},
			"Sets the physical device id to use."
			"Can be id, name or {igpu, dgpu, auto}", 1
		}
	}};
}

HeadlessArgs::HeadlessArgs(const argagg::parser_results& result) {
	layers = !result["no-validation"];
	renderdoc = result["renderdoc"];

	auto& rphdev = result["phdev"];
	if(rphdev.count() > 0) {
		try {
			phdev = rphdev.as<unsigned>();
		} catch(const std::exception&) {
			if(!std::strcmp(rphdev[0].arg, "auto")) {
				phdev = DevType::choose;
			} else if(!std::strcmp(rphdev[0].arg, "igpu")) {
				phdev = DevType::igpu;
			} else if(!std::strcmp(rphdev[0].arg, "dgpu")) {
				phdev = DevType::dgpu;
			} else {
				phdev = rphdev[0].arg;
			}
		}
	} else {
		phdev = DevType::choose;
	}
}

} // namespace tkn
