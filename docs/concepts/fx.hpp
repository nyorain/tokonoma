#pragma once

#include <tkn/types.hpp>
#include <nytl/span.hpp>

#include <vpp/fwd.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vector>

namespace tkn {

struct Pass {
	std::vector<vpp::TrDsLayout> dsLayouts;
	vpp::PipelineLayout pipeLayout;
	vpp::Pipeline pipe;
	std::vector<vpp::TrDs> dss;
};

Pass createComputePass(nytl::Span<const u32> spv);
Pass createGraphicsPass(nytl::Span<const u32> vert,
	nytl::Span<const u32> frag, std::function<void(vpp::GraphicsPipelineInfo&)>);

} // namespace tkn

// impl
#include <spirv_reflect.h>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>

namespace tkn {

vk::DescriptorType toVk(SpvReflectDescriptorType type) {
	return static_cast<vk::DescriptorType>(type);
}

void checkThrow(SpvReflectResult res) {
	if(res != SPV_REFLECT_RESULT_SUCCESS) {
		// TODO: provide descriptive message
		auto msg = dlg::format("spir-v reflection failed: {}", (int) res);
		throw std::runtime_error(msg);
	}
}

template<typename V>
void resizeAtLeast(V& vec, std::size_t size) {
	if(vec.size() < size) {
		vec.resize(size);
	}
}

// TODO: validate limits?
// - push constant size
// - descriptor limits (max number of sets, bindings, resources)
// Meh, i guess doing it here doesn't give us anything. If it's done
// correctly, it's pre-checked anyways and otherwise the layers
// catch it.
Pass createComputePass(vpp::Device& dev, nytl::Span<const u32> spv) {
	const char* entryPointName = "main";

	SpvReflectShaderModule reflMod;
	auto res = spvReflectCreateShaderModule(spv.size() * sizeof(u32), spv.data(), &reflMod);
	checkThrow(res);

	auto modGuard = nytl::ScopeGuard([&]{
		spvReflectDestroyShaderModule(&reflMod);
	});

	Pass pass;

	// descriptor set layouts
	std::vector<vk::DescriptorSetLayout> dsLayouts;
	for(auto i = 0u; i < reflMod.descriptor_set_count; ++i) {
		auto& set = reflMod.descriptor_sets[i];
		vk::DescriptorSetLayoutCreateInfo ci;

		std::vector<vk::DescriptorSetLayoutBinding> bindings;
		for(auto i = 0u; i < set.binding_count; ++i) {
			auto& binding = *set.bindings[i];

			vk::DescriptorSetLayoutBinding info;
			info.binding = binding.binding;
			info.descriptorCount = binding.count;
			info.stageFlags = vk::ShaderStageBits::compute;
			info.descriptorType = toVk(binding.descriptor_type);
			// info.pImmutableSamplers // TODO

			bindings.push_back(info);
		}

		ci.bindingCount = set.binding_count;
		ci.pBindings = bindings.data();

		resizeAtLeast(pass.dsLayouts, set.set);
		resizeAtLeast(dsLayouts, set.set);
		pass.dsLayouts[set.set].init(dev, ci);
		dsLayouts[set.set] = pass.dsLayouts[set.set];
	}

	// pipeline layout
	vk::PipelineLayoutCreateInfo plci;
	plci.pSetLayouts = dsLayouts.data();
	plci.setLayoutCount = dsLayouts.size();

	// push constant ranges
	auto blockVar = spvReflectGetEntryPointPushConstantBlock(&reflMod,
		entryPointName, &res);
	checkThrow(res);
	if(blockVar) {
		vk::PushConstantRange pcr;
		pcr.offset = blockVar->offset;
		pcr.size = blockVar->size;
		pcr.stageFlags = vk::ShaderStageBits::compute;

		plci.pushConstantRangeCount = 1u;
		plci.pPushConstantRanges = &pcr;
	}

	pass.pipeLayout = {dev, plci};

	// pipeline
	vpp::ShaderModule mod(dev, spv);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pass.pipeLayout;
	cpi.stage.module = mod;
	cpi.stage.pName = entryPointName;
	// TODO: allow specialization

	pass.pipe = {dev, cpi};

	pass.dss.reserve(pass.dsLayouts.size());
	for(auto& dsLayout : pass.dsLayouts) {
		if(!dsLayout.vkHandle()) {
			pass.dss.push_back({});
			continue;
		}

		pass.dss.emplace_back(dev.descriptorAllocator(), dsLayout);
	}

	return pass;
}

} // namespace tkn
