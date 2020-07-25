#pragma once

#include <tkn/types.hpp>
#include <tkn/fswatch.hpp>
#include <nytl/span.hpp>

#include <vpp/fwd.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vector>
#include <future>
#include <array>

namespace tkn {

struct PipelineState {
	std::vector<vpp::TrDsLayout> dsLayouts;
	vpp::PipelineLayout pipeLayout;
	std::vector<vpp::TrDs> dss;
};

struct ReloadableGraphicsPipeline {
	using InfoHandler = std::function<void(vpp::GraphicsPipelineInfo&)>;
	struct Shader {
		std::string path;
		std::uint64_t fileWatcherID;
	};

	vpp::Pipeline pipe;
	Shader vert;
	Shader frag;
	tkn::FileWatcher* watcher;
	std::future<vpp::Pipeline> future;
	InfoHandler infoHandler;

	void reload();
	void update();
	bool updateDevice();
};

struct ReloadableComputePipeline {
	using InfoHandler = std::function<void(vk::ComputePipelineCreateInfo&)>;

	std::string shaderPath;
	std::uint64_t fileWatcherID;
	vpp::Pipeline pipe;
	tkn::FileWatcher* watcher;
	std::future<vpp::Pipeline> future;
	InfoHandler infoHandler;

	void reload();
	void update();
	bool updateDevice();
};

PipelineState inferComputeState(nytl::Span<const u32> spv);
PipelineState inferGraphicsState(nytl::Span<const u32> vert,
	nytl::Span<const u32> frag, std::function<void(vpp::GraphicsPipelineInfo&)>);

class ComputePipelineState : public PipelineState {
public:
	ComputePipelineState(std::string shaderPath, tkn::FileWatcher* fw);

	void reload();
	void update();
	bool updateDevice();

protected:
	tkn::FileWatcher* fileWatcher_;
	std::string shaderPath_;
	ReloadableComputePipeline pipe_;
};

class GraphicsPipelineState : public PipelineState {
public:
	using InfoHandler = std::function<void(vpp::GraphicsPipelineInfo&)>;

public:
	void reload();
	void update();
	bool updateDevice();

protected:
	InfoHandler infoHandler_;
	std::string vertShaderPath_;
	std::string fragShaderPath_;
	ReloadableGraphicsPipeline pipe_;
};

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
PipelineState inferComputeState(vpp::Device& dev, nytl::Span<const u32> spv) {
	const char* entryPointName = "main";

	SpvReflectShaderModule reflMod;
	auto res = spvReflectCreateShaderModule(spv.size() * sizeof(u32), spv.data(), &reflMod);
	checkThrow(res);

	auto modGuard = nytl::ScopeGuard([&]{
		spvReflectDestroyShaderModule(&reflMod);
	});

	PipelineState state;

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

		resizeAtLeast(state.dsLayouts, set.set);
		resizeAtLeast(dsLayouts, set.set);
		state.dsLayouts[set.set].init(dev, ci);
		dsLayouts[set.set] = state.dsLayouts[set.set];
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

	state.pipeLayout = {dev, plci};

	// pipeline
	vpp::ShaderModule mod(dev, spv);

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = state.pipeLayout;
	cpi.stage.module = mod;
	cpi.stage.pName = entryPointName;
	// TODO: allow specialization

	state.dss.reserve(state.dsLayouts.size());
	for(auto& dsLayout : state.dsLayouts) {
		if(!dsLayout.vkHandle()) {
			state.dss.push_back({});
			continue;
		}

		state.dss.emplace_back(dev.descriptorAllocator(), dsLayout);
	}

	return state;
}

} // namespace tkn
