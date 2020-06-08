#pragma once

#include <tkn/defer.hpp>
#include <tkn/bits.hpp>
#include <tkn/texture.hpp>
#include <tkn/types.hpp>
#include <tkn/render.hpp>

#include <vpp/vk.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/handles.hpp>
#include <vpp/image.hpp>

// Utilities for modular render passes.

using namespace tkn::types;

// those were previously defined here, don't wanna change
// it everywhere.
using tkn::SyncScope;
using tkn::ComputeGroupSizeSpec;
using tkn::ImageBarrier;
using tkn::barrier;

struct PassCreateInfo {
	tkn::WorkBatcher& wb;
	vk::Format depthFormat;

	struct {
		const vpp::TrDsLayout& camera;
		const vpp::TrDsLayout& scene;
		const vpp::TrDsLayout& light;
	} dsLayouts;

	struct {
		vk::Sampler linear;
		vk::Sampler nearest;
	} samplers;

	vk::ShaderModule fullscreenVertShader;
};

