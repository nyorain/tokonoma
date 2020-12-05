// Sketch of a modular engine so I prototyping can be done even quicker,
// especially for connecting existing components

// ehat would be needed to render multiple vulkan frames at the same time?
// - each frame might need it's own ssbos, (offscreen) render targets etc
//   already not really viable...
// what would be needed to at least avoid the updateDevice choke point?
// - keep alive all descriptors and buffers etc, no real recreation possible in updateDevice
// - mapping device resources not possible. To would need at least two buffer spans
//   per buffer that might be updated between frames
//   Readback would be a bit harder.
// Does not really seem worth it, instead just make the updateDevice choke point
// small. But that probably needs a lot of the same things: creating
// resources already in update, pushing the old resources into a keep-alive
// manager (e.g. using std::any). Maybe doing it as described above *is* worth it?
//
// But not rerecording every frame sure makes it a lot harder, especially readback.
// We can't write the same resource in every frame. But we can't know which
// image we will get from the swapchain. Maybe have an updateDevice phase *only*
// for readback? Something like readbackDev()? That can be sure that currently
// no frame is rendered. But it should not do anything else.
// When are new command buffers recorded? Well, if we notice at the end of the
// update phase that we need it, we simply allocate new ones, push the old
// ones to the keep-alive manager (or just back into a managed pool) and record
// the new ones. Those are the ones we submit then. This alone is probably a
// really important improvement over the old updateDevice way.

// So, a frame should probably look like this:
// - acquire the next swapchain image
// - update all systems
// - then: check if redraw or rerecord are needed.
//   - if no redraw is needed, go back to updating all systems (possibly sleeping first)
//   - if a rerecord is needed, let the frame graph system record the current cb (lazily, on-demand)
// - submit the work to be done this frame, with a semaphore waiting
//   for the previous frame
// - queue a present (waiting on our previous submission)
// - wait for the previous (not this) frame on cpu side using a fence.
//   (we could do work here instead but should probably leave the heavy
//   stuff to other threads anyways and it's important we can wake up
//   as soon as possible after the fence is done)

// So, how to readback a buffer? It has to happen in our game tick.
// 1. We could simply move all reading of resources to its own command buffer
//    recording that is done every frame (well, lazily, if needed). And then
//    the systems can cycle their internal readback buffers.
// 2. We could synchronize access to the range using vulkan events.
//    But that would really kill the whole cpu-gpu parallelism, baaad!
// 3. Systems that need readback really need to have one readback
//    range per swapchain index (i.e. pre-recorded command buffer).
//    This could be done even if system 1. exists, depending on whether
//    its viable given the amount of memory read. In Engine we should
//    definitely keep track of the swapchain indices that are finished.
//    From the start on we can probably guarantee (via vulkan guarantees)
//    that we will always have the same number of buffers.

// Header
// #pragma once

#include <memory>
#include <any>
#include <nytl/vec.hpp>
#include <vpp/fwd.hpp>

namespace argagg { // fwd
	struct parser;
	struct parser_results;
} // namespace argagg


class Engine {
public:
	static Engine& get();

public:
	Engine(nytl::Span<const char*> args);
	~Engine();

	void run();

	void scheduleRedraw() const;
	void scheduleRerecord() const;

	// receive information
	double worldTime() const; // in seconds, pauses if app is paused
	double realTime() const; // in seconds, never pauses

	double dtReal() const; // in seconds, real delta time
	double dtWorld() const; // in seconds, world delta time, might be paused.

	nytl::Vec2ui windowSize() const;
	bool hasSurface() const;

protected:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

// Systems are *always* singletons. They are created by the engine
// but should expose access to their single instance. Therefore, care must
// be taken when accessing them in multithreaded scenarios (i.e. when the code
// isn't run during the main thread in update).
class System {
public:
	virtual ~System() = default;

	// Logic that might effect other systems should go here.
	virtual void preTick() {}
	// The main update logic should go here.
	virtual void tick() {}
	// Logic that depends on other systems being updated this frame goes here.
	virtual void postTick() {}
};

/*
class KeepAliveSystem : public System {
public:
	static KeepAliveSystem& get();

public:
	KeepAliveSystem() = default;

	template<typename T>
	void push(T&& obj) {
		pushAny(std::move(obj));
	}

	void pushAny(std::any obj);

protected:
	std::vector<std::any> keepAlive_;
};
*/

class PipelineSystem : public System {
public:
	static PipelineSystem& get();

public:
	PipelineSystem();

	void tick() override;

protected:
	std::vector<tkn::ReloadablePipeline*> pipelines_;
};

class FileWatchSystem : public System {
};

class LocalShadowSystem : public System {
protected:
	vpp::ViewableImage atlas;
};

// Use an improved version of the frame graph in deferred/graph.hpp

// cpp
#include <swa/swa.h>
#include <vpp/handles.hpp>
#include <vpp/device.hpp>
#include <vpp/swapchain.hpp>
#include <vector>
#include <array>

struct RenderBuffer {
	unsigned int id {};
	vk::Image image {};
	vk::CommandBuffer commandBuffer {};
	vpp::ImageView imageView;
};

struct FrameData {
	vpp::Semaphore semaphore;
	vpp::Fence fence;
};

struct Engine::Impl {
	// swa, display window
	swa_display* display;
	swa_window* window;

	// vulkan
	vpp::Instance instance;
	std::unique_ptr<vpp::Device> device;

	struct {
		const vpp::Queue* frameQueue; // used for frame graph
		const vpp::Queue* presentQueue;

		const vpp::Queue* transfer; // async transfer
		const vpp::Queue* compute; // async compute
	} queues;

	vpp::Swapchain swapchain;
	vpp::CommandPool commandPool;

	vpp::Semaphore acquireSemaphore;

	std::vector<RenderBuffer> renderBuffers;
	std::array<FrameData, 2> frameData;
	unsigned frameIndex {}; // 0 or 1,

	// frame graph

	// all systems
	std::vector<std::unique_ptr<System>> systems;
};

