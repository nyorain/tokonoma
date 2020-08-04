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

// IDEA: make descriptor updating easier, automatically applied
// 	in updateDevice or the PipelineState classes.
// TODO: add mechanism for descriptor sharing
// TODO: add general mechnism for callbacks executed when a new pipe is
//   loaded?

namespace tkn {

template<typename Sig> class Callable;
template<typename Sig> using UniqueCallable = std::unique_ptr<Callable<Sig>>;

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

	// Function that creates the pipeline from the set of loaded shader
	// stages. Note that this function might be called from a different
	// thread. In case of error, this function can simply throw.
	using Creator = Callable<vpp::Pipeline(const vpp::Device&,
		nytl::Span<const CompiledStage>) const>;

public:
	ReloadablePipeline() = default;
	ReloadablePipeline(const vpp::Device& dev, std::vector<Stage> stages,
		std::unique_ptr<Creator> creator, FileWatcher&,
		std::string name = {}, bool async = true);
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

	// TODO: allow changing name later on. It must be done in main thread
	// anyways since string can't be referenced through a move.
	// Add a general free-func `nameHandle` overload?
	// remove name parameter from constructor?
	const std::string& name() const { return name_; }

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
	static CreateInfo recreate(const vpp::Device& dev,
		nytl::Span<const Stage> stages, const Creator& creator);

private:
	const vpp::Device* dev_ {}; // need to store explicitly for async init
	std::unique_ptr<Creator> creator_;
	vpp::Pipeline pipe_;
	vpp::Pipeline newPipe_;

	std::vector<Stage> stages_;
	std::vector<Watch> fileWatches_;
	tkn::FileWatcher* fileWatcher_;
	std::future<CreateInfo> future_;

	std::string name_;
};

struct PipelineState {
	std::deque<vpp::TrDsLayout> dsLayouts;
	vpp::PipelineLayout pipeLayout;
	std::vector<vpp::TrDs> dss;
};

// TODO: allow better sampler mapping. Callback function or something
PipelineState inferComputeState(const vpp::Device& dev, nytl::Span<const u32> spv,
	vk::Sampler sampler = {});
PipelineState inferGraphicsState(const vpp::Device& dev, nytl::Span<const u32> vert,
	nytl::Span<const u32> frag, vk::Sampler sampler = {});

void cmdBindGraphics(vk::CommandBuffer cb, const PipelineState& state);
void cmdBindCompute(vk::CommandBuffer, const PipelineState& state);

// TODO: add name parameters
class ComputePipelineState {
public:
	using InfoHandler = Callable<void(vk::ComputePipelineCreateInfo&) const>;

public:
	ComputePipelineState() = default;
	ComputePipelineState(const vpp::Device&, std::string shaderPath,
		tkn::FileWatcher&, std::string preamble = {},
		std::unique_ptr<InfoHandler> infoHandler = {}, bool async = true);

	void reload() { pipe_.reload(); }
	void update() { pipe_.update(); }
	bool updateDevice() { return pipe_.updateDevice(); }

	auto& pipeState() { return *state_; }
	const auto& pipeState() const { return *state_; }
	auto& reloadablePipe() { return pipe_; }
	const auto& reloadablePipe() const { return pipe_; }
	vk::Pipeline pipe() const { return pipe_.pipe(); }
	vk::PipelineLayout pipeLayout() const { return state_->pipeLayout; }

protected:
	static vpp::Pipeline recreate(const vpp::Device& dev,
		nytl::Span<const ReloadablePipeline::CompiledStage> stages,
		PipelineState& state, const InfoHandler* infoHandler);

	std::unique_ptr<PipelineState> state_;
	std::unique_ptr<InfoHandler> infoHandler_;
	ReloadablePipeline pipe_;
};

class GraphicsPipelineState {
public:
	using InfoHandler = Callable<void(vpp::GraphicsPipelineInfo&) const>;

	struct StageInfo {
		std::string path;
		std::string preamble {};
	};

public:
	GraphicsPipelineState() = default;
	GraphicsPipelineState(const vpp::Device&,
		StageInfo vert, StageInfo frag, tkn::FileWatcher&,
		std::unique_ptr<InfoHandler>, bool async = true);

	void reload() { pipe_.reload(); }
	void update() { pipe_.update(); }
	bool updateDevice() { return pipe_.updateDevice(); }

	auto& pipeState() { return *state_; }
	const auto& pipeState() const { return *state_; }
	auto& reloadablePipe() { return pipe_; }
	const auto& reloadablePipe() const { return pipe_; }
	vk::Pipeline pipe() const { return pipe_.pipe(); }
	vk::PipelineLayout pipeLayout() const { return state_->pipeLayout; }

protected:
	static vpp::Pipeline recreate(const vpp::Device& dev,
		nytl::Span<const ReloadablePipeline::CompiledStage> stages,
		PipelineState& state, const InfoHandler& infoHandler);

	std::unique_ptr<PipelineState> state_;
	std::unique_ptr<InfoHandler> infoHandler_;
	ReloadablePipeline pipe_;
};

void cmdBind(vk::CommandBuffer cb, const ComputePipelineState&);
void cmdBind(vk::CommandBuffer cb, const GraphicsPipelineState&);

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

