#pragma once

#include <nytl/span.hpp>
#include <nytl/nonCopyable.hpp>

#include <ny/fwd.hpp>
#include <vpp/fwd.hpp>
#include <rvg/fwd.hpp>
#include <vui/fwd.hpp>

#include <memory>
#include <variant>
#include <string>
#include <optional>

namespace argagg {
	struct parser;
	struct parser_results;
} // namespace argagg

namespace doi {

// fwd
class MainWindow;
class Renderer;
struct RendererCreateInfo;

struct AppSettings {
	const char* name;
	nytl::Span<const char*> args;
};

/// Implements basic setup and main loop.
class App : public nytl::NonMovable {
public:
	App();
	virtual ~App();

	virtual bool init(const AppSettings& settings);
	virtual void run();

	ny::AppContext& appContext() const;
	MainWindow& window() const;
	Renderer& renderer() const;

	vpp::Instance& vulkanInstance() const;
	vpp::Device& vulkanDevice() const;

	rvg::Context& rvgContext() const;
	rvg::Transform& windowTransform() const;
	vui::Gui& gui() const;

	vk::SampleCountBits samples() const;

protected:
	// info
	// If overwritten by derived class to return true, will create default
	// framebuffer and renderpass with a depth buffer as attachment 1
	virtual bool needsDepth() const { return false; }

	// Can be overriden to use custom renderer implementation
	virtual std::unique_ptr<Renderer> createRenderer(const RendererCreateInfo&);

	// argument parsing
	virtual argagg::parser argParser() const;
	virtual bool handleArgs(const argagg::parser_results&);

	virtual bool features(vk::PhysicalDeviceFeatures& enable,
		const vk::PhysicalDeviceFeatures& supported);

	// recording
	virtual void render(vk::CommandBuffer);
	virtual void beforeRender(vk::CommandBuffer);
	virtual void afterRender(vk::CommandBuffer);

	// frame
	virtual void update(double dt);
	virtual void updateDevice();
	virtual void frameFinished() {}

	[[deprecated("Use scheduleRerecord")]]
	void rerecord() { rerecord_ = true; }
	void scheduleRerecord() { rerecord_ = true; }

	[[deprecated("Use scheduleRedraw")]]
	void redraw() { redraw_ = true; }
	void scheduleRedraw() { redraw_ = true; }

	void addSemaphore(vk::Semaphore, vk::PipelineStageFlags waitDst);
	void callUpdate();

	// events
	virtual void resize(const ny::SizeEvent&);
	virtual bool key(const ny::KeyEvent&);
	virtual bool mouseButton(const ny::MouseButtonEvent&);
	virtual void mouseMove(const ny::MouseMoveEvent&);
	virtual bool mouseWheel(const ny::MouseWheelEvent&);
	virtual void mouseCross(const ny::MouseCrossEvent&);
	virtual void focus(const ny::FocusEvent&);
	virtual void close(const ny::CloseEvent&);

protected:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	bool run_ {};
	bool resize_ {};
	bool rerecord_ {};
	bool redraw_ {};

	enum class DevType {
		igpu,
		dgpu,
		choose
	};

	struct {
		bool vsync = true;
		bool layers = true;
		bool renderdoc = false;
		unsigned samples = 1;
		std::variant<DevType, unsigned, const char*> phdev = DevType::dgpu;
	} args_ {};
};

// Utility function that compiles a shader using glslangValidator
// Useful for live shader reloading/switching
// Adds default include paths (src/shaders/{., include})
// Returns none on failure. glslPath should be given relative to "src/shaders/",
// so e.g. be just "particles.comp"
std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
	std::string_view glslPath);

} // namespace doi
