#pragma once

#include <tkn/types.hpp>
#include <tkn/fswatch.hpp>
#include <nytl/span.hpp>

#include <vpp/fwd.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vector>
#include <future>
#include <deque>
#include <array>

namespace tkn {

constexpr auto shaderEntryPointName = "main";

struct PipelineState {
	std::deque<vpp::TrDsLayout> dsLayouts;
	vpp::PipelineLayout pipeLayout;
	std::vector<vpp::TrDs> dss;
};

void cmdBindGraphics(vk::CommandBuffer cb, const PipelineState& state);
void cmdBindCompute(vk::CommandBuffer, const PipelineState& state);

struct CreatedPipelineInfo {
	vpp::Pipeline pipe;
	std::vector<std::string> included;
};

// TODO: classes are currently not movable. That kinda sucks, might be useful.
// TODO: stop watching in constructor. These should be full classes, not structs
struct ReloadableGraphicsPipeline {
	using InfoHandler = std::function<void(vpp::GraphicsPipelineInfo&)>;

	vpp::Pipeline pipe;
	vpp::Pipeline newPipe; // Will be applied in next updateDevice call.
	std::string vert;
	std::string frag;
	std::vector<std::uint64_t> fileWatches;
	tkn::FileWatcher* watcher;
	std::future<CreatedPipelineInfo> future;
	InfoHandler infoHandler;

	void init(const vpp::Device& dev, std::string vert, std::string frag,
		InfoHandler, FileWatcher&);
	void reload();
	void update();
	bool updateDevice();
};

struct ReloadableComputePipeline {
	using InfoHandler = std::function<void(vk::ComputePipelineCreateInfo&)>;

	std::string shaderPath;
	std::vector<std::uint64_t> fileWatches;
	vpp::Pipeline pipe;
	tkn::FileWatcher* watcher;
	std::future<vpp::Pipeline> future;
	InfoHandler infoHandler;

	// TODO
	// void init(const vpp::Device& dev, std::string path, FileWatcher&);
	void reload();
	void update();
	bool updateDevice();
};

PipelineState inferComputeState(const vpp::Device& dev, nytl::Span<const u32> spv,
	vk::Sampler sampler = {});
PipelineState inferGraphicsState(const vpp::Device& dev, nytl::Span<const u32> vert,
	nytl::Span<const u32> frag, vk::Sampler sampler = {});

class ComputePipelineState {
public:
	ComputePipelineState() = default;
	ComputePipelineState(const vpp::Device&, std::string shaderPath, tkn::FileWatcher&);

	void reload() { pipe_.reload(); }
	void update() { pipe_.update(); }
	bool updateDevice() { return pipe_.updateDevice(); }

	auto& pipeState() { return state_; }
	const auto& pipeState() const { return state_; }

	auto& reloadablePipe() { return pipe_; }
	const auto& reloadablePipe() const { return pipe_; }

	vk::Pipeline pipe() const { return pipe_.pipe; }
	vk::PipelineLayout pipeLayout() const { return state_.pipeLayout; }

protected:
	PipelineState state_;
	ReloadableComputePipeline pipe_;
};

class GraphicsPipelineState {
public:
	using InfoHandler = std::function<void(vpp::GraphicsPipelineInfo&)>;

public:
	GraphicsPipelineState() = default;
	GraphicsPipelineState(const vpp::Device&, std::string vertPath,
		std::string fragPath, InfoHandler, tkn::FileWatcher&);

	GraphicsPipelineState(GraphicsPipelineState&&) = delete;
	GraphicsPipelineState& operator=(GraphicsPipelineState&&) = delete;

	void init(const vpp::Device&, std::string vertPath,
		std::string fragPath, InfoHandler, tkn::FileWatcher&);

	void reload() { pipe_.reload(); }
	void update() { pipe_.update(); }
	bool updateDevice() { return pipe_.updateDevice(); }

	auto& pipeState() { return state_; }
	const auto& pipeState() const { return state_; }

	auto& reloadablePipe() { return pipe_; }
	const auto& reloadablePipe() const { return pipe_; }

	vk::Pipeline pipe() const { return pipe_.pipe; }
	vk::PipelineLayout pipeLayout() const { return state_.pipeLayout; }

protected:
	ReloadableGraphicsPipeline pipe_;
	PipelineState state_;
};

} // namespace tkn

