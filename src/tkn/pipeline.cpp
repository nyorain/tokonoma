#include <tkn/pipeline.hpp>
#include <tkn/threadPool.hpp>
#include <tkn/shader.hpp>
#include <vpp/vk.hpp>
#include <vpp/debug.hpp>
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

		auto& dsAlloc = ThreadState::instance(dev).dsAlloc();
		state.dss.emplace_back(dsAlloc, dsLayout);
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
		auto& set = reflVert.descriptor_sets[i];
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
			if(info.stageFlags) { // was already seen before
				dlg_assert(info.binding == binding.binding);
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

// ReloadablePipeline
ReloadablePipeline::ReloadablePipeline(const vpp::Device& dev,
	std::vector<Stage> stages, std::unique_ptr<Creator> creator,
	FileWatcher& fswatch, std::string name, bool async) :
		dev_(&dev), creator_(std::move(creator)),
		stages_(std::move(stages)), fileWatcher_(&fswatch),
		name_(std::move(name)) {

	dlg_assert(!stages_.empty());
	fileWatches_.reserve(stages.size());
	for(auto& stage : stages_) {
		auto resolved = ShaderCache::instance(dev).resolve(stage.file);
		if(resolved.empty()) {
			auto msg = dlg::format("Can't find shader '{}'", stage.file);
			throw std::runtime_error(msg);
		}

		fileWatches_.push_back({fileWatcher_->watch(resolved.c_str()), stage.file});
	}

	if(!async) {
		auto info = recreate(dev, nytl::Span<const Stage>(stages_), *creator_);
		pipe_ = std::move(info.pipe);
		if(!name_.empty()) {
			vpp::nameHandle(pipe_, name_.c_str());
		}

		updateWatches(std::move(info.included));
	} else {
		reload();
	}
}

ReloadablePipeline::~ReloadablePipeline() {
	for(auto& watch : fileWatches_) {
		fileWatcher_->unregsiter(watch.id);
	}
}

void ReloadablePipeline::reload() {
	// NOTE: because of this, we might miss changes when `reload` is
	// triggered while a reload job is till pending. This isn't really
	// a case we are too interested in though. We could set a flag here
	// to instantly start another reload job when the current is ready.
	// We could also just start multiple jobs (i.e. abandon the current
	// future) but ShaderCache was not really designed for it (would
	// probably expose some bugs).
	if(future_.valid()) {
		return;
	}

	auto& tp = ThreadPool::instance();

	// NOTE: we have to make sure that all resources we pass to `recreate`
	// stay valid even if *this is moved.
	future_ = tp.addPromised(&ReloadablePipeline::recreate,
		*dev_, nytl::Span<const Stage>(stages_), *creator_);
}

void ReloadablePipeline::update() {
	if(!future_.valid()) {
		if(fileWatcher_) {
			for(auto watch : fileWatches_) {
				if(fileWatcher_->check(watch.id)) {
					reload();
				}
			}
		}

		return;
	}

	// check if future is available
	auto res = future_.wait_for(std::chrono::seconds(0));
	if(res != std::future_status::ready) {
		return;
	}

	try {
		auto [newPipe, newInc] = future_.get(); // this might throw when task threw
		if(!newPipe) {
			return;
		}

		if(!name_.empty()) {
			vpp::nameHandle(newPipe_, name_.c_str());
		}

		newPipe_ = std::move(newPipe);
		updateWatches(std::move(newInc));
	} catch(const std::exception& err) {
		dlg_warn("Exception thrown from pipeline creation task: {}", err.what());
	}
}

bool ReloadablePipeline::updateDevice() {
	if(newPipe_) {
		pipe_ = std::move(newPipe_);
		return true;
	}

	return false;
}

void ReloadablePipeline::updateWatches(std::vector<std::string> newIncludes) {
	auto itOld = fileWatches_.begin() + stages_.size();
	for(auto& inc : newIncludes) {
		Watch* newWatch;
		if(itOld != fileWatches_.end()) {
			if(itOld->path == inc) {
				++itOld;
				continue;
			}

			fileWatcher_->unregsiter(itOld->id);
			newWatch = &*itOld;
			++itOld;
		} else {
			newWatch = &fileWatches_.emplace_back();
			itOld = fileWatches_.end();
		}

		newWatch->id = fileWatcher_->watch(inc);
		newWatch->path = std::move(inc);
	}
}

ReloadablePipeline::CreateInfo ReloadablePipeline::recreate(const vpp::Device& dev,
		nytl::Span<const Stage> stages, const Creator& creator) {
	CreateInfo ret;

	dlg_assert(!stages.empty());

	auto& sc = ShaderCache::instance(dev);
	std::vector<CompiledStage> cstages;
	cstages.reserve(stages.size());

	for(auto& stage : stages) {
		auto& cstage = cstages.emplace_back();
		cstage.stage = stage;
		cstage.module = sc.load(stage.file, stage.preamble);
		if(!cstage.module.mod) {
			return {};
		}

		auto includes = sc.resolveIncludes(stage.file);
		ret.included.insert(ret.included.end(), includes.begin(), includes.end());
	}

	ret.pipe = creator(dev, cstages);
	return ret;
}

// ComputePipelineState
ComputePipelineState::ComputePipelineState(const vpp::Device& dev,
		std::string shaderPath, tkn::FileWatcher& fswatch,
		std::string preamble, std::unique_ptr<InfoHandler> infoHandler,
		bool async) {
	ReloadablePipeline::Stage stage = {
		vk::ShaderStageBits::compute,
		std::move(shaderPath),
		std::move(preamble),
	};

	infoHandler_ = std::move(infoHandler);
	state_ = std::make_unique<PipelineState>();

	auto creatorFunc = [&state = *state_, infoHandler = infoHandler_.get()] (
			const vpp::Device& dev,
			nytl::Span<const ReloadablePipeline::CompiledStage> stages) {
		return recreate(dev, stages, state, infoHandler);
	};
	pipe_ = {dev, {stage}, makeUniqueCallable(creatorFunc), fswatch, {}, async};
}

vpp::Pipeline ComputePipelineState::recreate(const vpp::Device& dev,
		nytl::Span<const ReloadablePipeline::CompiledStage> stages,
		PipelineState& state, const InfoHandler* infoHandler) {
	dlg_assert(stages.size() == 1);
	auto& stage = stages[0];
	dlg_assert(stage.stage.stage == vk::ShaderStageBits::compute);

	// NOTE: this is not threadsafe! First (async) creation must not overlap
	// with a reload
	if(!state.pipeLayout) {
		state = inferComputeState(dev, stage.module.spv);
	}

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = state.pipeLayout;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = shaderEntryPointName;
	cpi.stage.module = stage.module.mod;
	if(infoHandler) {
		(*infoHandler)(cpi);
	}

	auto cache = PipelineCache::instance(dev);
	return vpp::Pipeline(dev, cpi, cache);
}

void cmdBind(vk::CommandBuffer cb, const ComputePipelineState& state) {
	dlg_assert(cb);
	dlg_assert(state.pipe());

	cmdBindCompute(cb, state.pipeState());
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, state.pipe());
}

// GraphicsPipelineState
GraphicsPipelineState::GraphicsPipelineState(const vpp::Device& dev,
		StageInfo vert, StageInfo frag, tkn::FileWatcher& fswatch,
		std::unique_ptr<InfoHandler> infoHandler, bool async) {
	std::vector<ReloadablePipeline::Stage> stages = {{
		vk::ShaderStageBits::vertex,
		std::move(vert.path),
		std::move(vert.preamble),
	}, {
		vk::ShaderStageBits::fragment,
		std::move(frag.path),
		std::move(frag.preamble),
	}};

	dlg_assert(infoHandler);
	infoHandler_ = std::move(infoHandler);
	state_ = std::make_unique<PipelineState>();

	auto creatorFunc = [&state = *state_, &infoHandler = *infoHandler_.get()] (
			const vpp::Device& dev,
			nytl::Span<const ReloadablePipeline::CompiledStage> stages) {
		return recreate(dev, stages, state, infoHandler);
	};
	pipe_ = {dev, std::move(stages), makeUniqueCallable(creatorFunc), fswatch,
		{}, async};
}

vpp::Pipeline GraphicsPipelineState::recreate(const vpp::Device& dev,
		nytl::Span<const ReloadablePipeline::CompiledStage> stages,
		PipelineState& state, const InfoHandler& infoHandler) {
	vpp::ShaderProgram program;
	nytl::Span<const u32> vertSpv;
	nytl::Span<const u32> fragSpv;
	for(auto& stage : stages) {
		program.stage({stage.module.mod, stage.stage.stage});

		// TODO: support other stages here and in inferGraphicsState
		if(stage.stage.stage == vk::ShaderStageBits::vertex) {
			dlg_assert(vertSpv.empty());
			dlg_assert(!stage.module.spv.empty());
			vertSpv = stage.module.spv;
		} else if(stage.stage.stage == vk::ShaderStageBits::fragment) {
			dlg_assert(fragSpv.empty());
			dlg_assert(!stage.module.spv.empty());
			fragSpv = stage.module.spv;
		} else {
			dlg_error("Unexpected shader stage {}", (unsigned) stage.stage.stage);
			return {};
		}
	}

	dlg_assert(!vertSpv.empty());
	dlg_assert(!fragSpv.empty());

	// NOTE: this is not threadsafe! First (async) creation must not overlap
	// with a reload
	if(!state.pipeLayout) {
		state = inferGraphicsState(dev, vertSpv, fragSpv);
	}

	vpp::GraphicsPipelineInfo gpi;
	gpi.layout(state.pipeLayout);
	gpi.program = std::move(program);
	infoHandler(gpi);

	auto cache = PipelineCache::instance(dev);
	return vpp::Pipeline(dev, gpi.info(), cache);
}

void cmdBind(vk::CommandBuffer cb, const GraphicsPipelineState& state) {
	dlg_assert(cb);
	dlg_assert(state.pipe());

	cmdBindGraphics(cb, state.pipeState());
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, state.pipe());
}

// ThreadState
class ThreadStateManager {
public:
	std::mutex mutex;
	std::unordered_map<std::thread::id, ThreadState*> states;

	static ThreadStateManager& instance() {
		static ThreadStateManager ini;
		return ini;
	}
};

ThreadState& ThreadState::instance(const vpp::Device& dev) {
	static thread_local ThreadState ini(dev, ThreadStateManager::instance());
	return ini;
}

void ThreadState::finishInstance() {
	auto& mgr = ThreadStateManager::instance();
	auto lock = std::lock_guard(mgr.mutex);
	for(auto& cache : mgr.states) {
		cache.second->destroy();
	}

	mgr.states.clear();
}

ThreadState::ThreadState(const vpp::Device& dev, ThreadStateManager& mgr) :
		dsAlloc_(dev) {
	auto lock = std::lock_guard(mgr.mutex);
	auto [it, emplaced] = mgr.states.try_emplace(std::this_thread::get_id(), this);
	dlg_assert(emplaced);
}

ThreadState::~ThreadState() {
	auto& mgr = ThreadStateManager::instance();
	auto lock = std::lock_guard(mgr.mutex);
	auto it = mgr.states.find(std::this_thread::get_id());
	if(it == mgr.states.end()) {
		dlg_error("~ThreadState: not present in manager list");
		return;
	}

	mgr.states.erase(it);
}

} // namespace tkn

