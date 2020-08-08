#pragma once

#include <tkn/types.hpp>
#include <tkn/fswatch.hpp>
#include <tkn/shader.hpp>
#include <tkn/callable.hpp>
#include <nytl/span.hpp>

#include <vpp/fwd.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/devMemAllocator.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vector>
#include <future>
#include <deque>
#include <array>

// TODO: add mechanism for descriptor sharing
// TODO: add general mechnism for callbacks executed when a new pipe is
//   loaded?

namespace tkn {

struct SpecializationInfo;
constexpr auto shaderEntryPointName = "main";

class ReloadablePipeline {
public:
	struct Stage {
		vk::ShaderStageBits stage;
		std::string file; // PERF: store path instead to avoid resolve calls
		std::string preamble {};
	};

	struct CompiledStage {
		Stage stage;
		ShaderCache::CompiledShaderView module;
	};

	struct CreatorInfo {
		const vpp::Device& dev;
		nytl::Span<const CompiledStage> stages;
		std::string_view pipeName;
	};

	// Function that creates the pipeline from the set of loaded shader
	// stages. Note that this function might be called from a different
	// thread. In case of error, this function can simply throw.
	using Creator = vpp::Pipeline(const CreatorInfo&) const;

public:
	ReloadablePipeline() = default;
	ReloadablePipeline(const vpp::Device& dev, std::vector<Stage> stages,
		UniqueCallable<Creator> creator, FileWatcher&,
		bool async = true, std::string name = {});
	~ReloadablePipeline();

	ReloadablePipeline(ReloadablePipeline&& rhs) = default;
	ReloadablePipeline& operator=(ReloadablePipeline&& rhs) = default;

	// Immediately starts a pipeline reloading.
	// The function does not wait for it to complete, the shader will be
	// loaded and the pipeline be created in a different thread.
	// If a reload job is already pending, this has no effect.
	void reload();

	// Should be called every frame (or at least every now and then). Does
	// cpu-work related to pipeline creation such as checking for file
	// change or completed creation jobs.
	void update();

	// Will update the used pipeline and potentially destroy the old one
	// when a job has finished. Returns whether the pipeline changed,
	// in which case no pipeline handle previously obtained from this
	// must be used anymore (i.e. command buffers be rerecorded).
	bool updateDevice();

	vk::Pipeline pipe() const { return pipe_; }
	const std::vector<Stage>& stages() const { return stages_; }
	bool updatePending() const { return newPipe_; }

	void name(std::string newName);
	const auto& name() const { return name_; }

private:
	struct CreateInfo {
		vpp::Pipeline pipe;
		std::vector<std::string> included;
	};

	struct Watch {
		std::uint64_t id;
		std::string path;
	};

	void updateWatches(std::vector<std::string> newIncludes);

	// Important that recreate gets its own string instead of referencing
	// this->name_, since that is not immutable (and moving it may invalidate
	// references, e.g. due to small string optimization, allowed by C++).
	static CreateInfo recreate(const vpp::Device& dev,
		nytl::Span<const Stage> stages, const Callable<Creator>& creator,
		std::string name);

private:
	const vpp::Device* dev_ {}; // need to store explicitly for async init
	UniqueCallable<Creator> creator_;
	vpp::Pipeline pipe_;
	vpp::Pipeline newPipe_;

	std::vector<Stage> stages_;
	std::vector<Watch> fileWatches_;
	tkn::FileWatcher* fileWatcher_;
	std::future<CreateInfo> future_;

	std::string name_;
};

struct LayoutedDs {
	std::vector<vk::DescriptorSetLayoutBinding> bindings;
	vpp::TrDsLayout layout;
	vpp::TrDs ds;
};

struct PipeLayoutDescriptors {
	vpp::PipelineLayout pipeLayout;
	std::deque<LayoutedDs> descriptors;
};

// TODO: filter out redundant updates
// TODO: clean up implementation.
// 	Maybe always use abstract updates and only resolve in 'update'?
// TODO: support all descriptor types
class DescriptorUpdater {
public:
	// Does *not* apply updates in its destructor
	DescriptorUpdater() = default;
	~DescriptorUpdater() = default;

	DescriptorUpdater(DescriptorUpdater&&) = default;
	DescriptorUpdater& operator=(DescriptorUpdater&&) = default;

	DescriptorUpdater(const DescriptorUpdater&) = delete;
	DescriptorUpdater& operator=(const DescriptorUpdater&) = delete;

	void write(const vk::WriteDescriptorSet&);
	void buffer(std::vector<vk::DescriptorBufferInfo>,
		std::optional<vk::DescriptorType> type,
		unsigned startElem = 0u);
	void image(std::vector<vk::DescriptorImageInfo>,
		std::optional<vk::DescriptorType> type,
		unsigned startElem = 0u);

	void buffer(vpp::BufferSpan span, std::optional<vk::DescriptorType> type);
	void image(vk::ImageView view, vk::ImageLayout, vk::Sampler,
		std::optional<vk::DescriptorType>);

	void uniformBuffer(vpp::BufferSpan span);
	void storageBuffer(vpp::BufferSpan span);
	void storageImage(vk::ImageView view,
		vk::ImageLayout = vk::ImageLayout::general);
	void sampledImage(vk::ImageView view,
		vk::ImageLayout = vk::ImageLayout::shaderReadOnlyOptimal);
	void combinedImageSampler(vk::ImageView view,
		vk::ImageLayout = vk::ImageLayout::shaderReadOnlyOptimal,
		vk::Sampler sampler = {});
	void inputAttachment(vk::ImageView view,
		vk::ImageLayout = vk::ImageLayout::shaderReadOnlyOptimal);
	void sampler(vk::Sampler sampler);

	void set(vpp::BufferSpan span);
	void set(vk::ImageView view,
		vk::ImageLayout = vk::ImageLayout::undefined,
		vk::Sampler = {});
	void set(const vpp::ViewableImage&,
		vk::ImageLayout = vk::ImageLayout::undefined,
		vk::Sampler = {});
	void set(vk::Sampler);

	template<typename... Args>
	void operator()(const Args&... args) {
		set(args...);
	}

	void seek(unsigned set, unsigned binding);
	void skip(unsigned inc = 1u);
	void nextSet(unsigned inc = 1u);

	/// Returns whether an update was performed.
	/// Simply does nothing when no updates are queued.
	bool apply();

	void init(const std::deque<LayoutedDs>& dss);
	bool initialized() const { return sets_; }

protected:
	struct AbstractUpdate {
		unsigned set;
		unsigned binding; // may go beyond set, into next set
		unsigned startElem {};
		std::variant<
			nytl::Span<vk::DescriptorBufferInfo>,
			nytl::Span<vk::DescriptorImageInfo>> data;
		std::optional<vk::DescriptorType> as;
	};

	std::vector<AbstractUpdate> abstractUpdates_;
	std::vector<vk::WriteDescriptorSet> writes_;

	// double vector to avoid reference (in writes_) invalidation
	// some values must be stored continuesly, so deque doesnt work
	std::vector<std::vector<vk::DescriptorBufferInfo>> buffers_;
	std::vector<std::vector<vk::BufferView>> views_;
	std::vector<std::vector<vk::DescriptorImageInfo>> images_;

	unsigned int currentBinding_ = 0;
	unsigned int currentSet_ = 0;
	const std::deque<LayoutedDs>* sets_ {};
};

using SamplerProvider = tkn::Callable<const vk::Sampler*(unsigned set, unsigned binding) const>;

PipeLayoutDescriptors inferComputeState(const vpp::Device& dev, nytl::Span<const u32> spv,
	vpp::DescriptorAllocator* dsAlloc = nullptr,
	const SamplerProvider* samplers = {});
PipeLayoutDescriptors inferGraphicsState(const vpp::Device& dev, nytl::Span<const u32> vert,
	nytl::Span<const u32> frag, vpp::DescriptorAllocator* dsAlloc = nullptr,
	const SamplerProvider* samplers = {});
void nameHandle(const PipeLayoutDescriptors&, std::string_view name);

void cmdBindGraphics(vk::CommandBuffer cb, const PipeLayoutDescriptors& state);
void cmdBindCompute(vk::CommandBuffer, const PipeLayoutDescriptors& state);

// ManagedPipelines
struct ComputePipeInfoProvider {
	static std::unique_ptr<ComputePipeInfoProvider> create(vk::Sampler);
	static std::unique_ptr<ComputePipeInfoProvider> create(
		tkn::SpecializationInfo, vk::Sampler sampler = {});

	virtual ~ComputePipeInfoProvider() = default;
	virtual void fill(vk::ComputePipelineCreateInfo&) const {}
	virtual const vk::Sampler* samplers(unsigned set, unsigned binding) const {
		(void) set; (void) binding;
		return {};
	}
};

class ManagedComputePipe {
public:
	using InfoProvider = ComputePipeInfoProvider;

public:
	ManagedComputePipe() = default;
	ManagedComputePipe(const vpp::Device&, std::string shaderPath,
		tkn::FileWatcher&, std::string preamble = {},
		std::unique_ptr<InfoProvider> provider = {}, bool async = true,
		std::string name = {});

	void reload() { pipe_.reload(); }
	void update() { pipe_.update(); }
	bool updateDevice();

	auto& pipeState() { return *state_; }
	const auto& pipeState() const { return *state_; }
	auto& reloadablePipe() { return pipe_; }
	const auto& reloadablePipe() const { return pipe_; }
	vk::Pipeline pipe() const { return pipe_.pipe(); }
	vk::PipelineLayout pipeLayout() const { return state_->pipeLayout; }

	auto& dsu() { return updater_; }

protected:
	static vpp::Pipeline recreate(const ReloadablePipeline::CreatorInfo& info,
		PipeLayoutDescriptors& state, const InfoProvider* infoHandler);

	std::unique_ptr<PipeLayoutDescriptors> state_;
	std::unique_ptr<InfoProvider> infoProvider_;
	ReloadablePipeline pipe_;
	DescriptorUpdater updater_;
};

struct GraphicsPipeInfoProvider {
	static std::unique_ptr<GraphicsPipeInfoProvider> create(
		vpp::GraphicsPipelineInfo info, vk::Sampler sampler = {});

	virtual ~GraphicsPipeInfoProvider() = default;
	virtual void fill(vpp::GraphicsPipelineInfo&) const = 0;
	virtual const vk::Sampler* samplers(unsigned set, unsigned binding) const {
		(void) set; (void) binding;
		return {};
	}
};

class ManagedGraphicsPipe {
public:
	using InfoProvider = GraphicsPipeInfoProvider;
	struct StageInfo {
		std::string path;
		std::string preamble {};
	};

public:
	ManagedGraphicsPipe() = default;
	ManagedGraphicsPipe(const vpp::Device&,
		StageInfo vert, StageInfo frag, tkn::FileWatcher&,
		std::unique_ptr<InfoProvider>, bool async = true,
		std::string name = {});

	void reload() { pipe_.reload(); }
	void update() { pipe_.update(); }
	bool updateDevice();

	auto& pipeState() { return *state_; }
	const auto& pipeState() const { return *state_; }
	auto& reloadablePipe() { return pipe_; }
	const auto& reloadablePipe() const { return pipe_; }
	vk::Pipeline pipe() const { return pipe_.pipe(); }
	vk::PipelineLayout pipeLayout() const { return state_->pipeLayout; }

	auto& dsu() { return updater_; }

protected:
	static vpp::Pipeline recreate(const ReloadablePipeline::CreatorInfo& info,
		PipeLayoutDescriptors& state, const InfoProvider& provider);

	std::unique_ptr<PipeLayoutDescriptors> state_;
	std::unique_ptr<InfoProvider> infoProvider_;
	ReloadablePipeline pipe_;
	DescriptorUpdater updater_;
};

void cmdBind(vk::CommandBuffer cb, const ManagedComputePipe&);
void cmdBind(vk::CommandBuffer cb, const ManagedGraphicsPipe&);

class ThreadStateManager;
class ThreadState {
public:
	static ThreadState& instance(const vpp::Device& dev);
	static void finishInstance();

public:
	vpp::DescriptorAllocator& dsAlloc() { return *dsAlloc_; }

private:
	friend class ThreadStateManager;
	ThreadState(const vpp::Device& dev, ThreadStateManager&);
	~ThreadState();

	void destroy() {
		dsAlloc_.reset();
	}

	std::optional<vpp::DescriptorAllocator> dsAlloc_;
};

} // namespace tkn
