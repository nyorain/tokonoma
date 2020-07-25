#include <tkn/shaderReload.hpp>
#include <tkn/threadPool.hpp>
#include <tkn/shader.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <nytl/scope.hpp>
#include <spirv_reflect.h>

namespace tkn {

vk::DescriptorType toVk(SpvReflectDescriptorType type) {
	return static_cast<vk::DescriptorType>(type);
}

void checkThrow(SpvReflectResult res, bool allowNotFound = false) {
	if(res != SPV_REFLECT_RESULT_SUCCESS &&
			(!allowNotFound || res != SPV_REFLECT_RESULT_ERROR_ELEMENT_NOT_FOUND)) {
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

bool needsSampler(vk::DescriptorType dsType) {
	switch(dsType) {
		case vk::DescriptorType::sampler:
		case vk::DescriptorType::combinedImageSampler:
			return true;
		default:
			return false;
	}
}

void cmdBindGraphics(vk::CommandBuffer cb, const PipelineState& state) {
	std::vector<vk::DescriptorSet> sets;
	for(auto& ds : state.dss) {
		sets.push_back(ds);
	}

	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		state.pipeLayout, 0, sets, {});
}

void cmdBindCompute(vk::CommandBuffer cb, const PipelineState& state) {
	std::vector<vk::DescriptorSet> sets;
	for(auto& ds : state.dss) {
		sets.push_back(ds);
	}

	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
		state.pipeLayout, 0, sets, {});
}

// TODO: validate limits?
// - push constant size
// - descriptor limits (max number of sets, bindings, resources)
// Meh, i guess doing it here doesn't give us anything. If it's done
// correctly, it's pre-checked anyways and otherwise the layers
// catch it.
PipelineState inferComputeState(const vpp::Device& dev, nytl::Span<const u32> spv,
		vk::Sampler sampler) {
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
			if(sampler && needsSampler(info.descriptorType)) {
				dlg_assert(info.descriptorCount == 1u);
				info.pImmutableSamplers = &sampler;
			}

			bindings.push_back(info);
		}

		ci.bindingCount = set.binding_count;
		ci.pBindings = bindings.data();

		resizeAtLeast(state.dsLayouts, set.set + 1);
		resizeAtLeast(dsLayouts, set.set + 1);
		state.dsLayouts[set.set].init(dev, ci);
		dsLayouts[set.set] = state.dsLayouts[set.set];
	}

	// pipeline layout
	vk::PipelineLayoutCreateInfo plci;
	plci.pSetLayouts = dsLayouts.data();
	plci.setLayoutCount = dsLayouts.size();

	// push constant ranges
	auto blockVar = spvReflectGetEntryPointPushConstantBlock(&reflMod,
		shaderEntryPointName, &res);
	checkThrow(res, true);
	if(blockVar) {
		vk::PushConstantRange pcr;
		pcr.offset = blockVar->offset;
		pcr.size = blockVar->size;
		pcr.stageFlags = vk::ShaderStageBits::compute;

		plci.pushConstantRangeCount = 1u;
		plci.pPushConstantRanges = &pcr;
	}

	state.pipeLayout = {dev, plci};

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

PipelineState inferGraphicsState(const vpp::Device& dev, nytl::Span<const u32> vert,
		nytl::Span<const u32> frag, vk::Sampler sampler) {
	SpvReflectShaderModule reflVert {};
	auto res0 = spvReflectCreateShaderModule(vert.size() * sizeof(u32), vert.data(), &reflVert);
	SpvReflectShaderModule reflFrag {};
	auto res1 = spvReflectCreateShaderModule(frag.size() * sizeof(u32), frag.data(), &reflFrag);

	auto modGuard = nytl::ScopeGuard([&]{
		spvReflectDestroyShaderModule(&reflVert);
		spvReflectDestroyShaderModule(&reflFrag);
	});

	checkThrow(res0);
	checkThrow(res1);

	PipelineState state;

	// descriptor set layouts
	std::vector<std::vector<vk::DescriptorSetLayoutBinding>> bindings;
	for(auto i = 0u; i < reflVert.descriptor_set_count; ++i) {
		auto& set = reflFrag.descriptor_sets[i];
		resizeAtLeast(bindings, set.set + 1);

		for(auto i = 0u; i < set.binding_count; ++i) {
			auto& binding = *set.bindings[i];

			resizeAtLeast(bindings[set.set], binding.binding + 1);
			auto& info = bindings[set.set][binding.binding];

			info.binding = binding.binding;
			info.descriptorCount = binding.count;
			info.stageFlags = vk::ShaderStageBits::vertex;
			info.descriptorType = toVk(binding.descriptor_type);
			if(sampler && needsSampler(info.descriptorType)) {
				dlg_assert(info.descriptorCount == 1u);
				info.pImmutableSamplers = &sampler;
			}
		}
	}

	for(auto i = 0u; i < reflFrag.descriptor_set_count; ++i) {
		auto& set = reflFrag.descriptor_sets[i];
		resizeAtLeast(bindings, set.set + 1);

		for(auto i = 0u; i < set.binding_count; ++i) {
			auto& binding = *set.bindings[i];

			resizeAtLeast(bindings[set.set], binding.binding + 1);
			auto& info = bindings[set.set][binding.binding];
			if(info.binding != 0u) {
				info.descriptorCount = std::max(info.descriptorCount, binding.count);
				info.stageFlags |= vk::ShaderStageBits::fragment;
				if(info.descriptorType != toVk(binding.descriptor_type)) {
					throw std::runtime_error("Vertex/fragment shader bindings don't match");
				}
			} else {
				info.binding = binding.binding;
				info.descriptorCount = binding.count;
				info.stageFlags = vk::ShaderStageBits::fragment;
				info.descriptorType = toVk(binding.descriptor_type);
				if(sampler && needsSampler(info.descriptorType)) {
					dlg_assert(info.descriptorCount == 1u);
					info.pImmutableSamplers = &sampler;
				}
			}
		}
	}

	std::vector<vk::DescriptorSetLayout> dsLayouts;
	dsLayouts.resize(bindings.size());
	state.dsLayouts.resize(bindings.size());
	for(auto i = 0u; i < bindings.size(); ++i) {
		auto& setBindings = bindings[i];
		if(setBindings.empty()) {
			continue;
		}

		vk::DescriptorSetLayoutCreateInfo ci;
		ci.bindingCount = setBindings.size();
		ci.pBindings = setBindings.data();
		state.dsLayouts[i].init(dev, ci);
		dsLayouts[i] = state.dsLayouts[i];
	}

	// pipeline layout
	vk::PipelineLayoutCreateInfo plci;
	plci.pSetLayouts = dsLayouts.data();
	plci.setLayoutCount = dsLayouts.size();

	// push constant ranges
	auto vertBlockVar = spvReflectGetEntryPointPushConstantBlock(&reflVert,
		shaderEntryPointName, &res0);
	checkThrow(res0, true);

	auto fragBlockVar = spvReflectGetEntryPointPushConstantBlock(&reflFrag,
		shaderEntryPointName, &res1);
	checkThrow(res1, true);

	vk::PushConstantRange pcrs[2];
	auto pcrCount = 0u;
	if(vertBlockVar) {
		pcrs[pcrCount].offset = vertBlockVar->offset;
		pcrs[pcrCount].size = vertBlockVar->size;
		pcrs[pcrCount].stageFlags = vk::ShaderStageBits::vertex;
		++pcrCount;
	}

	if(fragBlockVar) {
		pcrs[pcrCount].offset = fragBlockVar->offset;
		pcrs[pcrCount].size = fragBlockVar->size;
		pcrs[pcrCount].stageFlags = vk::ShaderStageBits::fragment;
		++pcrCount;
	}

	plci.pushConstantRangeCount = pcrCount;
	plci.pPushConstantRanges = pcrs;
	state.pipeLayout = {dev, plci};

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

// ReloadableComputePipeline
/*
void ReloadableComputePipeline::reload() {
	// TODO: already schedule a new reload in this case?
	// abandon the current future? but we probably should not do it
	// while we are reloading it.
	if(future.valid()) {
		return;
	}

	auto& tp = ThreadPool::instance();
	future = tp.addPromised([this, &dev = pipe.device()]{
		auto& shaderCache = ShaderCache::instance(dev);

		std::vector<std::string> included;
		auto mod = shaderCache.load(this->shaderPath, {}, {}, &included);
		if(!mod) {
			return vpp::Pipeline {};
		}

		// pipeline
		vk::ComputePipelineCreateInfo cpi;
		cpi.stage.module = mod;
		cpi.stage.stage = vk::ShaderStageBits::compute;
		this->infoHandler(cpi);

		// TODO: add new includes, remove old ones.
		// We can't do this here (since it's run in a different thread),
		// should return this information to the main thread and
		// do it there (in update/updateDevice).

		return vpp::Pipeline(dev, cpi);
	});
}

void ReloadableComputePipeline::update() {
	if(!watcher || future.valid()) {
		return;
	}

	for(auto watch : fileWatches) {
		if(watcher->check(watch)) {
			reload();
		}
	}
}

bool ReloadableComputePipeline::updateDevice() {
	if(!future.valid()) {
		return false;
	}

	auto res = future.wait_for(std::chrono::seconds(0));
	if(res != std::future_status::ready) {
		return false;
	}

	try {
		auto newPipe = future.get(); // this might throw when task threw
		if(newPipe) {
			this->pipe = std::move(newPipe);
			return true;
		}
	} catch(const std::exception& err) {
		dlg_warn("Exception thrown from pipeline creation task: {}", err.what());
	}

	return false;
}
*/

// ReloadableGraphicsPipeline
// TODO: we make assumptions that certain members of 'pipe' are immtuable here
// (to allow this function in another thread)
CreatedPipelineInfo recreate(const vpp::Device& dev, ReloadableGraphicsPipeline& pipe) {
	auto& shaderCache = ShaderCache::instance(dev);

	std::vector<std::string> vertIncluded;
	auto vertMod = shaderCache.load(pipe.vert, {}, {}, &vertIncluded);
	if(!vertMod) {
		return {};
	}

	std::vector<std::string> fragIncluded;
	auto fragMod = shaderCache.load(pipe.frag, {}, {}, &fragIncluded);
	if(!fragMod) {
		return {};
	}

	// pipeline
	vpp::GraphicsPipelineInfo gpi;
	gpi.program = vpp::ShaderProgram{{{
		{vertMod, vk::ShaderStageBits::vertex},
		{fragMod, vk::ShaderStageBits::fragment},
	}}};

	pipe.infoHandler(gpi);

	auto included = std::move(vertIncluded);
	included.insert(included.end(), fragIncluded.begin(), fragIncluded.end());
	return {vpp::Pipeline(dev, gpi.info()), std::move(included)};
}

void ReloadableGraphicsPipeline::init(const vpp::Device& dev, std::string vert,
		std::string frag, InfoHandler infoHandler, FileWatcher& fileWatcher) {
	// auto& sc = ShaderCache::instance(dev);

	this->watcher = &fileWatcher;
	this->infoHandler = infoHandler;

	this->vert = std::move(vert);
	this->frag = std::move(frag);

	// this->vert = sc.resolve(vert);
	// if(this->vert.empty()) {
	// 	dlg_error("Can't find shader {}", vert);
	// 	throw std::runtime_error("Can't find shader");
	// }

	// this->frag = sc.resolve(frag);
	// if(this->frag.empty()) {
	// 	dlg_error("Can't find shader {}", frag);
	// 	throw std::runtime_error("Can't find shader");
	// }

	// TODO: make that async as well?
	auto [pipe, included] = recreate(dev, *this);
	this->pipe = std::move(pipe);

	// already in included.
	// fileWatches.push_back(fileWatcher.watch(this->vert));
	// fileWatches.push_back(fileWatcher.watch(this->frag));

	for(auto& inc : included) {
		fileWatches.push_back(fileWatcher.watch(inc));
	}
}

void ReloadableGraphicsPipeline::reload() {
	// TODO: already schedule a new reload in this case?
	// abandon the current future? but we probably should not do it
	// while we are reloading it.
	if(future.valid()) {
		return;
	}

	auto& tp = ThreadPool::instance();
	future = tp.addPromised(&recreate, pipe.device(), *this);
}

void ReloadableGraphicsPipeline::update() {
	if(!future.valid()) {
		if(watcher) {
			for(auto watch : fileWatches) {
				if(watcher->check(watch)) {
					reload();
				}
			}
		}

		return;
	}

	// check if future is available
	auto res = future.wait_for(std::chrono::seconds(0));
	if(res != std::future_status::ready) {
		return;
	}

	try {
		auto [newPipe, newInc] = future.get(); // this might throw when task threw
		if(!newPipe) {
			return;
		}

		this->newPipe = std::move(newPipe);

		// TODO(opt): don't always unregister and re-register.
		// Check first if something really has changed.
		for(auto i = 0u; i < fileWatches.size(); ++i) {
			watcher->unregsiter(fileWatches[i]);
		}

		// fileWatches.resize(2);
		// fileWatches.reserve(2 + newInc.size());
		fileWatches.clear();
		fileWatches.reserve(newInc.size());
		for(auto& inc : newInc) {
			fileWatches.push_back(watcher->watch(inc));
		}

	} catch(const std::exception& err) {
		dlg_warn("Exception thrown from pipeline creation task: {}", err.what());
	}
}

bool ReloadableGraphicsPipeline::updateDevice() {
	if(newPipe) {
		this->pipe = std::move(newPipe);
		return true;
	}

	return false;
}

// GraphicsPipelineState

// ComputePipelineState
/*
ComputePipelineState::ComputePipelineState(const vpp::Device& dev,
		std::string shader, FileWatcher& watcher) {
	auto& shaderCache = ShaderCache::instance(dev);

	std::vector<std::string> included;
	std::vector<u32> spv;
	auto mod = shaderCache.load(shader, {}, {}, &included, &spv);

	state_ = inferComputeState(spv);

	// pipeline
	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout();
	cpi.stage.module = mod;
	cpi.stage.pName = shaderEntryPointName;
	// TODO: allow specialization
	pipe_.pipe = {dev, cpi};

	pipe_.shaderPath = shader;
	pipe_.watcher = &watcher;
	pipe_.infoHandler = [layout = this->pipeLayout()]
			(vk::ComputePipelineCreateInfo& cpi){
		cpi.layout = layout;
		cpi.stage.pName = shaderEntryPointName;
	};

	pipe_.fileWatches.reserve(included.size() + 1);
	pipe_.fileWatches.push_back(watcher.watch(shader));
	for(auto& inc : included) {
		pipe_.fileWatches.push_back(watcher.watch(inc));
	}
}
*/

GraphicsPipelineState::GraphicsPipelineState(const vpp::Device& dev,
		std::string vertPath, std::string fragPath, InfoHandler handler,
		tkn::FileWatcher& watcher) {
	init(dev, vertPath, fragPath, handler, watcher);
}

void GraphicsPipelineState::init(const vpp::Device& dev,
		std::string vertPath, std::string fragPath, InfoHandler handler,
		tkn::FileWatcher& watcher) {

	// TODO: bad code duplication with ReloadableGraphicsPipeline::init
	pipe_.watcher = &watcher;
	pipe_.infoHandler = [this, handler = std::move(handler)]
			(vpp::GraphicsPipelineInfo& gpi) {
		gpi.layout(state_.pipeLayout);
		handler(gpi);
	};

	pipe_.vert = std::move(vertPath);
	pipe_.frag = std::move(fragPath);

	// auto& sc = ShaderCache::instance(dev);
	// pipe_.vert = sc.resolve(vertPath);
	// if(pipe_.vert.empty()) {
	// 	dlg_error("Can't find shader {}", vertPath);
	// 	throw std::runtime_error("Can't find shader");
	// }
//
	// pipe_.frag = sc.resolve(fragPath);
	// if(pipe_.frag.empty()) {
	// 	dlg_error("Can't find shader {}", fragPath);
	// 	throw std::runtime_error("Can't find shader");
	// }

	auto& shaderCache = ShaderCache::instance(dev);

	std::vector<std::string> vertIncluded;
	std::vector<u32> vertSpv;
	auto vertMod = shaderCache.load(pipe_.vert, {}, {}, &vertIncluded, &vertSpv);
	if(!vertMod) {
		throw std::runtime_error("Can't load shader");
	}

	std::vector<std::string> fragIncluded;
	std::vector<u32> fragSpv;
	auto fragMod = shaderCache.load(pipe_.frag, {}, {}, &fragIncluded, &fragSpv);
	if(!fragMod) {
		throw std::runtime_error("Can't load shader");
	}

	state_ = inferGraphicsState(dev, vertSpv, fragSpv);

	// pipeline
	vpp::GraphicsPipelineInfo gpi;
	gpi.program = vpp::ShaderProgram{{{
		{vertMod, vk::ShaderStageBits::vertex},
		{fragMod, vk::ShaderStageBits::fragment},
	}}};

	pipe_.infoHandler(gpi);

	auto included = std::move(vertIncluded);
	included.insert(included.end(), fragIncluded.begin(), fragIncluded.end());
	pipe_.pipe = vpp::Pipeline(dev, gpi.info());

	// pipe_.fileWatches.push_back(watcher.watch(pipe_.vert));
	// pipe_.fileWatches.push_back(watcher.watch(pipe_.frag));
	for(auto& inc : included) {
		pipe_.fileWatches.push_back(watcher.watch(inc));
	}
}

} // namespace tkn
