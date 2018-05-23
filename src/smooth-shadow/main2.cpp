// Copyright (c) 2017 nyorain
// Distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt

#include "light.hpp"
#include <stage/window.hpp>
#include <stage/render.hpp>
#include <argagg.hpp>

#include <vpp/instance.hpp>
#include <vpp/device.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/physicalDevice.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/debug.hpp>
#include <vpp/vk.hpp>

#include <ny/backend.hpp>
#include <ny/appContext.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/key.hpp>

#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>
#include <dlg/dlg.hpp>

#include <optional>

#include <shaders/fullscreen.vert.h>
#include <shaders/light_pp.frag.h>

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
	auto* kc = appContext->keyboardContext();

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

	// resources
	auto subBuf = vpp::SubBuffer(device.bufferAllocator(),
		sizeof(nytl::Mat4f), vk::BufferUsageBits::uniformBuffer,
		4u, device.hostMemoryTypes());

	auto viewLayoutBindings = {
		vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
			vk::ShaderStageBits::vertex)};
	auto viewLayout = vpp::TrDsLayout(device, viewLayoutBindings);
	auto viewDs = vpp::TrDs(device.descriptorAllocator(), viewLayout);
	vpp::DescriptorSetUpdate update(viewDs);
	update.uniform({{subBuf.buffer(), subBuf.offset(), subBuf.size()}});
	update.apply();

	LightSystem lightSystem(device, viewLayout);
	lightSystem.addSegment({{{1.f, 1.f}, {2.f, 1.f}}, -1.f});

	auto& light = lightSystem.addLight();
	light.position = {0.5f, 0.5f};

	auto& light1 = lightSystem.addLight();
	light1.position = {2.0f, 2.0f};

	auto currentLight = &light;

	// post-process/combine
	auto info = vk::SamplerCreateInfo {};
	info.maxAnisotropy = 1.0;
	info.magFilter = vk::Filter::linear;
	info.minFilter = vk::Filter::linear;
	info.minLod = 0;
	info.maxLod = 0.25;
	info.mipmapMode = vk::SamplerMipmapMode::nearest;
	auto sampler = vpp::Sampler(device, info);
	auto ppBindings = {
		vpp::descriptorBinding(vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, -1, 1, &sampler.vkHandle()),
	};

	auto ppLayout = vpp::TrDsLayout(device, ppBindings);
	auto pipeSets = {ppLayout.vkHandle()};

	vk::PipelineLayoutCreateInfo plInfo;
	plInfo.setLayoutCount = 1;
	plInfo.pSetLayouts = pipeSets.begin();
	auto ppPipeLayout = vpp::PipelineLayout {device, plInfo};

	auto combineVertex = vpp::ShaderModule(device, fullscreen_vert_data);
	auto combineFragment = vpp::ShaderModule(device, light_pp_frag_data);

	vpp::GraphicsPipelineInfo combinePipeInfo(renderer.renderPass(),
		ppPipeLayout, vpp::ShaderProgram({
			{combineVertex, vk::ShaderStageBits::vertex},
			{combineFragment, vk::ShaderStageBits::fragment}
	}));

	combinePipeInfo.assembly.topology = vk::PrimitiveTopology::triangleFan;

	vk::Pipeline vkPipe;
	vk::createGraphicsPipelines(device, {}, 1, combinePipeInfo.info(),
		nullptr, vkPipe);
	auto ppPipe = vpp::Pipeline(device, vkPipe);

	auto ppDs = vpp::TrDs(device.descriptorAllocator(), ppLayout);
	vpp::DescriptorSetUpdate ppDsUpdate(ppDs);
	ppDsUpdate.imageSampler({{{}, lightSystem.renderTarget().vkImageView(),
		vk::ImageLayout::shaderReadOnlyOptimal}});
	ppDsUpdate.apply();

	// main loop stuff
	auto transform = nytl::identity<4, float>();
	auto run = true;
	std::optional<nytl::Vec2ui> resize;
	window.onResize = [&](const auto& ev) {
		resize = ev.size;

		auto w = ev.size.x / float(ev.size.y);
		auto h = 1.f;
		auto fac = 10 / std::sqrt(w * w + h * h);

		auto s = nytl::Vec {
			(2.f / (fac * w)),
			(-2.f / (fac * h)), 1
			// 2.f / ev.size.x,
			// -2.f / ev.size.y, 1
		};

		transform = nytl::identity<4, float>();
		scale(transform, s);
		translate(transform, {-1, 1, 0});
	};

	window.onClose = [&](const auto&) {
		run = false;
	};


	renderer.beforeRender = [&](auto cmdBuf) {
		vk::cmdBindDescriptorSets(cmdBuf, vk::PipelineBindPoint::graphics,
			lightSystem.lightPipeLayout(), 0, {viewDs}, {});
		lightSystem.renderLights(cmdBuf);
	};

	renderer.onRender = [&](auto cmdBuf) {
		vk::cmdBindDescriptorSets(cmdBuf, vk::PipelineBindPoint::graphics,
			ppPipeLayout, 0, {ppDs}, {});
		vk::cmdBindPipeline(cmdBuf, vk::PipelineBindPoint::graphics, ppPipe);
		vk::cmdDraw(cmdBuf, 4, 1, 0, 0);
	};

	// initial event poll
	if(!appContext->pollEvents()) {
		dlg_info("pollEvents returned false");
		return 0;
	}

	using Clock = std::chrono::high_resolution_clock;
	using Secf = std::chrono::duration<float, std::ratio<1, 1>>;
	auto lastFrame = Clock::now();

	std::uint64_t frameID {};
	while(run) {
		// - update device -
		if(resize) {
			auto size = *resize;
			resize = {};
			renderer.resize(size);
			auto map = subBuf.memoryMap();
			std::memcpy(map.ptr(), &transform, sizeof(transform));
		}

		if(lightSystem.updateDevice()) {
			renderer.invalidate();
		}

		// - submit and present -
		auto wait = true;
		auto res = renderer.render(&frameID);
		if(res != vk::Result::success) {
			auto retry =
				res == vk::Result::suboptimalKHR ||
				res == vk::Result::errorOutOfDateKHR;
			if(retry) {
				dlg_debug("Skipping suboptimal/outOfDate frame");
				wait = false;
			} else {
				dlg_error("render error: {}", vk::name(res));
				return EXIT_FAILURE;
			}
		}

		// - update logical state, process events -
		auto now = Clock::now();
		auto diff = now - lastFrame;
		auto deltaCount = std::chrono::duration_cast<Secf>(diff).count();
		lastFrame = now;

		if(!appContext->pollEvents()) {
			dlg_info("pollEvents returned false");
			return 0;
		}

		if(currentLight) {
			auto fac = 2 * deltaCount;
			if(kc->pressed(ny::Keycode::d)) {
				currentLight->position += nytl::Vec {fac, 0.f};
			}
			if(kc->pressed(ny::Keycode::a)) {
				currentLight->position += nytl::Vec {-fac, 0.f};
			}
			if(kc->pressed(ny::Keycode::w)) {
				currentLight->position += nytl::Vec {0.f, fac};
			}
			if(kc->pressed(ny::Keycode::s)) {
				currentLight->position += nytl::Vec {0.f, -fac};
			}
		}

		lightSystem.update(deltaCount);

		// - wait for rendering to finish -
		if(wait) {
			device.queueSubmitter().wait(frameID);
		}
	}
}
