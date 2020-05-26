#pragma once

#include <vkpp/structs.hpp>

namespace tkn {

struct Features {
	vk::PhysicalDeviceFeatures2 base;
	vk::PhysicalDeviceMultiviewFeatures multiview;

	// TODO: currently implemented in app.cpp
	Features();
};

} // namespace tkn
