#include <tkn/pipeline.hpp>
#include <tkn/util.hpp>
#include <tkn/threadPool.hpp>
#include <tkn/render.hpp>
#include <tkn/shader.hpp>
#include <vpp/vk.hpp>
#include <vpp/image.hpp>
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

void cmdBindGraphics(vk::CommandBuffer cb, const PipeLayoutDescriptors& state) {
	if(state.descriptors.empty()) {
		return;
	}

	std::vector<vk::DescriptorSet> sets;
	for(auto& ds : state.descriptors) {
		sets.push_back(ds.ds);
	}

	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		state.pipeLayout, 0, sets, {});
}

void cmdBindCompute(vk::CommandBuffer cb, const PipeLayoutDescriptors& state) {
	if(state.descriptors.empty()) {
		return;
	}

	std::vector<vk::DescriptorSet> sets;
	for(auto& ds : state.descriptors) {
		sets.push_back(ds.ds);
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
PipeLayoutDescriptors inferComputeState(const vpp::Device& dev, nytl::Span<const u32> spv,
		vpp::DescriptorAllocator* dsAlloc, const SamplerProvider* samplers) {
	if(!dsAlloc) {
		dsAlloc = &dev.descriptorAllocator();
	}

	SpvReflectShaderModule reflMod;
	auto res = spvReflectCreateShaderModule(spv.size() * sizeof(u32), spv.data(), &reflMod);
	checkThrow(res);

	auto modGuard = nytl::ScopeGuard([&]{
		spvReflectDestroyShaderModule(&reflMod);
	});

	PipeLayoutDescriptors state;

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
			if(samplers && needsSampler(info.descriptorType)) {
				dlg_assert(info.descriptorCount == 1u);
				info.pImmutableSamplers = (*samplers)(binding.set, binding.binding);
			}

			bindings.push_back(info);
		}

		ci.bindingCount = set.binding_count;
		ci.pBindings = bindings.data();

		resizeAtLeast(state.descriptors, set.set + 1);
		resizeAtLeast(dsLayouts, set.set + 1);
		state.descriptors[set.set].layout.init(dev, ci);
		state.descriptors[set.set].bindings = std::move(bindings);
		dsLayouts[set.set] = state.descriptors[set.set].layout;
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

	for(auto& ds : state.descriptors) {
		if(ds.layout.vkHandle()) {
			ds.ds = {*dsAlloc, ds.layout};
		}
	}

	return state;
}

PipeLayoutDescriptors inferGraphicsState(const vpp::Device& dev, nytl::Span<const u32> vert,
		nytl::Span<const u32> frag, vpp::DescriptorAllocator* dsAlloc,
		const SamplerProvider* samplers) {
	if(!dsAlloc) {
		dsAlloc = &dev.descriptorAllocator();
	}

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

	PipeLayoutDescriptors state;

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
			if(samplers && needsSampler(info.descriptorType)) {
				dlg_assert(info.descriptorCount == 1u);
				info.pImmutableSamplers = (*samplers)(binding.set, binding.binding);
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
				if(samplers && needsSampler(info.descriptorType)) {
					dlg_assert(info.descriptorCount == 1u);
					info.pImmutableSamplers = (*samplers)(binding.set, binding.binding);
				}
			}
		}
	}

	std::vector<vk::DescriptorSetLayout> dsLayouts;
	dsLayouts.resize(bindings.size());
	state.descriptors.resize(bindings.size());
	for(auto i = 0u; i < bindings.size(); ++i) {
		auto& setBindings = bindings[i];
		if(setBindings.empty()) {
			continue;
		}

		vk::DescriptorSetLayoutCreateInfo ci;
		ci.bindingCount = setBindings.size();
		ci.pBindings = setBindings.data();
		state.descriptors[i].layout.init(dev, ci);
		state.descriptors[i].bindings = std::move(setBindings);
		dsLayouts[i] = state.descriptors[i].layout;
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

	for(auto& ds : state.descriptors) {
		if(ds.layout.vkHandle()) {
			ds.ds = {*dsAlloc, ds.layout};
		}
	}

	return state;
}

void nameHandle(const PipeLayoutDescriptors& state, std::string_view name) {
	dlg_assert(state.pipeLayout);

	auto pipeLayoutName = std::string(name) + ":pipeLayout";
	vpp::nameHandle(state.pipeLayout, pipeLayoutName.c_str());

	for(auto i = 0u; i < state.descriptors.size(); ++i) {
		auto dslName = dlg::format("{}:dsLayout[{}]", name, i);
		vpp::nameHandle(state.descriptors[i].layout, dslName.c_str());

		auto dsName = dlg::format("{}:ds[{}]", name, i);
		vpp::nameHandle(state.descriptors[i].ds, dsName.c_str());
	}
}

// ReloadablePipeline
ReloadablePipeline::ReloadablePipeline(const vpp::Device& dev,
	std::vector<Stage> stages, UniqueCallable<Creator> creator,
	FileWatcher& fswatch, bool async, std::string name) :
		dev_(&dev), creator_(std::move(creator)),
		stages_(std::move(stages)), fileWatcher_(&fswatch) {

	dlg_assert(!stages_.empty());
	fileWatches_.reserve(stages.size());
	for(auto& stage : stages_) {
		auto resolved = ShaderCache::instance(dev).resolve(stage.file);
		if(resolved.empty()) {
			auto msg = dlg::format("Can't find shader '{}'", stage.file);
			throw std::runtime_error(msg);
		}

		fileWatches_.push_back({fileWatcher_->watch(resolved.c_str()), stage.file});

		bool relevantStage = stage.stage == vk::ShaderStageBits::compute ||
			stage.stage == vk::ShaderStageBits::fragment;
		if(name.empty() && relevantStage) {
			name = stage.file;
		}
	}

	if(name.empty()) {
		name = stages[0].file;
	}

	name_ = name;
	if(!async) {
		auto info = recreate(dev, nytl::Span<const Stage>(stages_), *creator_, name_);
		pipe_ = std::move(info.pipe);
		updateWatches(std::move(info.included));
	} else {
		reload();
	}
}

ReloadablePipeline::~ReloadablePipeline() {
	if(future_.valid()) {
		try {
			// make sure to wait on future to complete.
			// This might throw when task threw
			future_.get();
		} catch(const std::exception& err) {
			dlg_warn("Exception thrown from pipeline creation task: {}", err.what());
		}
	}

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
		*dev_, nytl::Span<const Stage>(stages_), *creator_,
		name_ + "[new]");
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

		newPipe_ = std::move(newPipe);
		vpp::nameHandle(newPipe_, (name_ + "[pending]").c_str());
		updateWatches(std::move(newInc));
	} catch(const std::exception& err) {
		dlg_warn("Exception thrown from pipeline creation task: {}", err.what());
	}
}

bool ReloadablePipeline::updateDevice() {
	if(newPipe_) {
		pipe_ = std::move(newPipe_);
		vpp::nameHandle(pipe_, name_.c_str());
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
		nytl::Span<const Stage> stages, const Callable<Creator>& creator,
		std::string name) {
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

	// somewhat hacky, remove [new] tag from name if it exists
	auto endTag = std::string_view("[new]");
	auto pn = std::string_view(name);
	if(hasSuffix(pn, endTag)) {
		pn = pn.substr(0, pn.size() - endTag.size());
	}

	ret.pipe = creator({dev, cstages, pn});
	vpp::nameHandle(ret.pipe, name.c_str());

	return ret;
}

void ReloadablePipeline::name(std::string newName) {
	name_ = std::move(newName);
	vpp::nameHandle(pipe_, name_.c_str());
}

// ComputePipeLayoutDescriptors
std::unique_ptr<ComputePipeInfoProvider>
ComputePipeInfoProvider::create(tkn::SpecializationInfo info,
		vk::Sampler sampler) {
	struct Impl : public ComputePipeInfoProvider {
		vk::SpecializationInfo spec;
		tkn::SpecializationInfo specData;
		vk::Sampler sampler;

		void fill(vk::ComputePipelineCreateInfo& cpi) const override {
			if(spec.mapEntryCount) {
				cpi.stage.pSpecializationInfo = &spec;
			}
		}

		const vk::Sampler* samplers(unsigned, unsigned) const override {
			return sampler ? &sampler : nullptr;
		}

		Impl(tkn::SpecializationInfo xinfo, vk::Sampler xsampler) {
			specData = std::move(xinfo);
			spec = specData.info();
			sampler = xsampler;
		}
	};

	return std::make_unique<Impl>(std::move(info), sampler);
}

std::unique_ptr<ComputePipeInfoProvider>
ComputePipeInfoProvider::create(vk::Sampler sampler) {
	return create(tkn::SpecializationInfo{}, sampler);
}

ManagedComputePipe::ManagedComputePipe(const vpp::Device& dev,
		std::string shaderPath, tkn::FileWatcher& fswatch,
		std::string preamble, std::unique_ptr<InfoProvider> provider,
		bool async, std::string name) {
	ReloadablePipeline::Stage stage = {
		vk::ShaderStageBits::compute,
		std::move(shaderPath),
		std::move(preamble),
	};

	infoProvider_ = std::move(provider);
	state_ = std::make_unique<PipeLayoutDescriptors>();

	auto creatorFunc = [&state = *state_, infoProvider = infoProvider_.get()]
			(const ReloadablePipeline::CreatorInfo& info) {
		return recreate(info, state, infoProvider);
	};

	pipe_ = {dev, {stage}, creatorFunc, fswatch, async, std::move(name)};
}

vpp::Pipeline ManagedComputePipe::recreate(
		const ReloadablePipeline::CreatorInfo& info,
		PipeLayoutDescriptors& state, const InfoProvider* provider) {

	dlg_assert(info.stages.size() == 1);
	auto& stage = info.stages[0];
	dlg_assert(stage.stage.stage == vk::ShaderStageBits::compute);

	// NOTE: this is not threadsafe! First (async) creation must not overlap
	// with a reload
	if(!state.pipeLayout) {
		auto& dsAlloc = ThreadState::instance(info.dev).dsAlloc();
		auto samplers = CallableImpl([&](unsigned set, unsigned binding) {
			return provider ? provider->samplers(set, binding) : nullptr;
		});

		state = inferComputeState(info.dev, stage.module.spv, &dsAlloc, &samplers);
		nameHandle(state, info.pipeName);
	}

	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = state.pipeLayout;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = shaderEntryPointName;
	cpi.stage.module = stage.module.mod;
	if(provider) {
		provider->fill(cpi);
	}

	auto cache = PipelineCache::instance(info.dev);
	return vpp::Pipeline(info.dev, cpi, cache);
}

bool ManagedComputePipe::updateDevice() {
	bool ret = pipe_.updateDevice();
	if(ret && !updater_.initialized()) {
		updater_.init(state_->descriptors);
	}

	ret |= updater_.apply();
	return ret;
}

void cmdBind(vk::CommandBuffer cb, const ManagedComputePipe& state) {
	dlg_assert(cb);
	dlg_assert(state.pipe());

	cmdBindCompute(cb, state.pipeState());
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, state.pipe());
}

// GraphicsPipelineState
std::unique_ptr<GraphicsPipeInfoProvider> GraphicsPipeInfoProvider::create(
		vpp::GraphicsPipelineInfo info, vk::Sampler sampler) {
	struct Impl : public GraphicsPipeInfoProvider {
		vpp::GraphicsPipelineInfo gpi;
		vk::Sampler sampler;

		void fill(vpp::GraphicsPipelineInfo& ngpi) const override {
			auto prog = std::move(ngpi.program);
			auto layout = ngpi.info().layout;
			ngpi = gpi;
			ngpi.program = std::move(prog);
			ngpi.layout(layout);
		}

		const vk::Sampler* samplers(unsigned, unsigned) const override {
			return sampler ? &sampler : nullptr;
		}

		Impl(vpp::GraphicsPipelineInfo xgpi, vk::Sampler xsampler) {
			gpi = std::move(xgpi);
			sampler = xsampler;
		}
	};

	return std::make_unique<Impl>(std::move(info), sampler);
}

ManagedGraphicsPipe::ManagedGraphicsPipe(const vpp::Device& dev,
		StageInfo vert, StageInfo frag, tkn::FileWatcher& fswatch,
		std::unique_ptr<InfoProvider> infoProvider,
		bool async, std::string name) {
	std::vector<ReloadablePipeline::Stage> stages = {{
		vk::ShaderStageBits::vertex,
		std::move(vert.path),
		std::move(vert.preamble),
	}, {
		vk::ShaderStageBits::fragment,
		std::move(frag.path),
		std::move(frag.preamble),
	}};

	dlg_assert(infoProvider);
	infoProvider_ = std::move(infoProvider);
	state_ = std::make_unique<PipeLayoutDescriptors>();

	auto creatorFunc = [&state = *state_, &infoProvider = *infoProvider_.get()]
		(const ReloadablePipeline::CreatorInfo& info) {
		return recreate(info, state, infoProvider);
	};
	pipe_ = {dev, std::move(stages), creatorFunc, fswatch,
		async, std::move(name)};
}

vpp::Pipeline ManagedGraphicsPipe::recreate(
		const ReloadablePipeline::CreatorInfo& info,
		PipeLayoutDescriptors& state, const InfoProvider& provider) {
	dlg_assert(info.stages.size() >= 2);

	vpp::ShaderProgram program;
	nytl::Span<const u32> vertSpv;
	nytl::Span<const u32> fragSpv;
	for(auto& stage : info.stages) {
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
		auto& dsAlloc = ThreadState::instance(info.dev).dsAlloc();
		auto samplers = CallableImpl([&](unsigned set, unsigned binding) {
			return provider.samplers(set, binding);
		});

		state = inferGraphicsState(info.dev, vertSpv, fragSpv, &dsAlloc, &samplers);
		nameHandle(state, info.pipeName);
	}

	vpp::GraphicsPipelineInfo gpi;
	gpi.layout(state.pipeLayout);
	gpi.program = std::move(program);
	provider.fill(gpi);

	auto cache = PipelineCache::instance(info.dev);
	return vpp::Pipeline(info.dev, gpi.info(), cache);
}

bool ManagedGraphicsPipe::updateDevice() {
	bool ret = pipe_.updateDevice();
	if(ret && !updater_.initialized()) {
		updater_.init(state_->descriptors);
	}

	ret |= updater_.apply();
	return ret;
}

void cmdBind(vk::CommandBuffer cb, const ManagedGraphicsPipe& state) {
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

// DescriptorUpdater
bool areRangesOverlapping(unsigned startA, unsigned countA,
		unsigned startB, unsigned countB) {
	return (startA >= startB && startA < startB + countB) ||
		(startB >= startA && startB < startA + countA);
}

void DescriptorUpdater::write(const vk::WriteDescriptorSet& write) {
	// check if there is already a write
	for(auto& w : writes_) {
		if(w.dstSet == write.dstSet &&
				w.dstBinding == write.dstBinding &&
				areRangesOverlapping(w.dstArrayElement, w.descriptorCount,
					write.dstArrayElement, write.descriptorCount)) {
			// TODO: implement array case
			dlg_assert(write.descriptorCount == 1);
			dlg_assert(w.descriptorCount == 1);
			w = write;
			return;
		}
	}

	// TODO: validate (only information that validation layer does not provide).
	// E.g. make sure that no sampler was given when descriptor type
	// does not need it (output warning).
	writes_.push_back(write);
}

void DescriptorUpdater::buffer(std::vector<vk::DescriptorBufferInfo> infos,
		std::optional<vk::DescriptorType> type, unsigned startElem) {

	buffers_.push_back(std::move(infos));
	if(!sets_) {
		auto& update = abstractUpdates_.emplace_back();
		update.data = nytl::Span<vk::DescriptorBufferInfo>(buffers_.back().data(), buffers_.back().size());
		update.set = currentSet_;
		update.binding = currentBinding_;
		update.startElem = startElem;
		update.as = type;
	} else {
		auto& set = (*sets_)[currentSet_];
		auto dt = set.bindings[currentBinding_].descriptorType;
		dlg_assert(!type || dt == *type);
		dlg_assert(set.bindings[currentBinding_].descriptorCount >= infos.size());

		write({set.ds, currentBinding_,
			startElem, u32(buffers_.back().size()), dt, nullptr,
			buffers_.back().data(), nullptr});
	}

	skip();
}

void DescriptorUpdater::image(std::vector<vk::DescriptorImageInfo> infos,
		std::optional<vk::DescriptorType> type, unsigned startElem) {

	images_.push_back(std::move(infos));
	if(!sets_) {
		auto& update = abstractUpdates_.emplace_back();
		update.data = images_.back();
		update.set = currentSet_;
		update.binding = currentBinding_;
		update.startElem = startElem;
		update.as = type;
	} else {
		auto& set = (*sets_)[currentSet_];
		auto dt = set.bindings[currentBinding_].descriptorType;
		dlg_assert(!type || dt == *type);
		dlg_assert(set.bindings[currentBinding_].descriptorCount >= infos.size());

		write({set.ds, currentBinding_,
			startElem, u32(images_.back().size()), dt, images_.back().data(),
			nullptr, nullptr});
	}

	skip();
}

void DescriptorUpdater::buffer(vpp::BufferSpan span,
		std::optional<vk::DescriptorType> type) {
	vk::DescriptorBufferInfo info;
	info.buffer = span.buffer();
	info.offset = span.offset();
	info.range = span.size();
	this->buffer({info}, type);
}

void DescriptorUpdater::image(vk::ImageView view, vk::ImageLayout layout,
		vk::Sampler sampler, std::optional<vk::DescriptorType> type) {
	vk::DescriptorImageInfo info;
	info.imageView = view;
	info.imageLayout = layout;
	info.sampler = sampler;
	this->image({info}, type);
}

void DescriptorUpdater::uniformBuffer(vpp::BufferSpan span) {
	buffer(span, vk::DescriptorType::uniformBuffer);
}

void DescriptorUpdater::storageBuffer(vpp::BufferSpan span) {
	buffer(span, vk::DescriptorType::storageBuffer);
}
void DescriptorUpdater::storageImage(vk::ImageView view, vk::ImageLayout layout) {
	image(view, layout, {}, vk::DescriptorType::storageImage);
}
void DescriptorUpdater::combinedImageSampler(vk::ImageView view,
		vk::ImageLayout layout, vk::Sampler sampler) {
	image(view, layout, sampler, vk::DescriptorType::combinedImageSampler);
}
void DescriptorUpdater::inputAttachment(vk::ImageView view, vk::ImageLayout layout) {
	image(view, layout, {}, vk::DescriptorType::inputAttachment);
}
void DescriptorUpdater::sampler(vk::Sampler sampler) {
	image({}, {}, sampler, vk::DescriptorType::sampler);
}

void DescriptorUpdater::sampledImage(vk::ImageView view, vk::ImageLayout layout) {
	image(view, layout, {}, vk::DescriptorType::sampledImage);
}

void DescriptorUpdater::set(vpp::BufferSpan span) {
	if(sets_) {
		auto& set = (*sets_)[currentSet_];
		buffer(span, set.bindings[currentBinding_].descriptorType);
	} else {
		buffer(span, {});
	}
}

void DescriptorUpdater::set(vk::ImageView view,
		vk::ImageLayout layout, vk::Sampler sampler) {
	if(sets_) {
		auto& set = (*sets_)[currentSet_];
		if(layout == vk::ImageLayout::undefined) {
			switch(set.bindings[currentBinding_].descriptorType) {
				case vk::DescriptorType::combinedImageSampler:
				case vk::DescriptorType::sampledImage:
					layout = vk::ImageLayout::shaderReadOnlyOptimal;
					break;
				case vk::DescriptorType::storageImage:
					layout = vk::ImageLayout::general;
					break;
				default:
					break;
			}
		}

		image(view, layout, sampler, set.bindings[currentBinding_].descriptorType);
	} else {
		image(view, layout, sampler, {});
	}
}
void DescriptorUpdater::set(const vpp::ViewableImage& img,
		vk::ImageLayout layout, vk::Sampler sampler) {
	set(img.imageView(), layout, sampler);
}

void DescriptorUpdater::set(vk::Sampler sampler) {
	set({}, {}, sampler);
}

void DescriptorUpdater::set(vk::ImageView view, vk::Sampler sampler) {
	set(view, vk::ImageLayout::undefined, sampler);
}

void DescriptorUpdater::set(const vpp::ViewableImage& img, vk::Sampler sampler) {
	set(img.vkImageView(), vk::ImageLayout::undefined, sampler);
}

void DescriptorUpdater::seek(unsigned set, unsigned binding) {
	currentSet_ = set;
	currentBinding_ = binding;
	if(!sets_) {
		return;
	}

	while(currentSet_ < sets_->size() &&
			currentBinding_ >= (*sets_)[currentSet_].bindings.size()) {
		currentBinding_ -= (*sets_)[currentSet_].bindings.size();
		++currentSet_;
	}
}

void DescriptorUpdater::nextSet(unsigned inc) {
	currentSet_ += inc;
	currentBinding_ = 0;
}

void DescriptorUpdater::skip(unsigned inc) {
	currentBinding_ += inc;
	if(!sets_) {
		return;
	}

	while(currentSet_ < sets_->size() &&
			currentBinding_ >= (*sets_)[currentSet_].bindings.size()) {
		currentBinding_ -= (*sets_)[currentSet_].bindings.size();
		++currentSet_;
	}
}

bool DescriptorUpdater::apply() {
	currentSet_ = 0u;
	currentBinding_ = 0u;

	if(!sets_ || writes_.empty()) {
		return false;
	}

	auto& dev = (*sets_)[0].layout.device();
	vk::updateDescriptorSets(dev, writes_, {});

	writes_.clear();
	images_.clear();
	buffers_.clear();
	views_.clear();

	return true;
}

void DescriptorUpdater::init(const std::deque<LayoutedDs>& dss) {
	dlg_assert(!sets_);

	sets_ = &dss;
	for(auto& abstract : abstractUpdates_) {
		seek(abstract.set, abstract.binding);
		auto& set = (*sets_)[currentSet_];
		auto type = set.bindings[currentBinding_].descriptorType;
		dlg_assert(!abstract.as || *abstract.as == type);

		std::visit(Visitor{
			[&](nytl::Span<vk::DescriptorBufferInfo> infos) {
				write({set.ds, currentBinding_,
					abstract.startElem, u32(infos.size()), type, nullptr,
					infos.data(), nullptr});
			},
			[&](nytl::Span<vk::DescriptorImageInfo> infos) {
				// fix undefined layouts
				for(auto& info : infos) {
					if(info.imageLayout == vk::ImageLayout::undefined) {
						switch(type) {
							case vk::DescriptorType::combinedImageSampler:
							case vk::DescriptorType::sampledImage:
								info.imageLayout = vk::ImageLayout::shaderReadOnlyOptimal;
								break;
							case vk::DescriptorType::storageImage:
								info.imageLayout = vk::ImageLayout::general;
								break;
							default:
								break;
						}
					}
				}

				write({set.ds, currentBinding_,
					abstract.startElem, u32(infos.size()), type, infos.data(),
					nullptr, nullptr});
			},
		}, abstract.data);
	}

	abstractUpdates_.clear();
	currentSet_ = 0u;
	currentBinding_ = 0u;
}

} // namespace tkn

