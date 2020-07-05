#pragma once

#include <optional>
#include <vpp/fwd.hpp>
#include <vpp/shader.hpp>
#include <nytl/stringParam.hpp>

#include <vector>
#include <unordered_map>
#include <array>
#include <filesystem>
namespace fs = std::filesystem;

namespace tkn {

// Forward to compileShader. This one is just badly named.
[[deprecated("Use ShaderCache or 'compileShader' instead")]]
std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
	std::string_view glslPath, nytl::StringParam args = {});

// Utility function that compiles a shader using glslangValidator
// Useful for live shader reloading/switching.
// Adds default include paths (src/shaders/ include)
// Returns none on failure. glslPath should be given relative to "src/",
// so e.g. be just "particles/particles.comp"
// Prefer using ShaderCache which holds a cache of compiled shader modules
// on disk and in memory.
std::optional<vpp::ShaderModule> compileShader(const vpp::Device& dev,
	std::string_view glslPath, nytl::StringParam args = {});

class ShaderCache {
public:
	static ShaderCache& instance();
	// TODO: replace this with some platform-specific shader cache dir
	static constexpr auto cacheDir = "shadercache/";

public:
	std::vector<std::string> includePaths = {
		TKN_BASE_DIR "/src/",
		TKN_BASE_DIR "/src/shaders/include/",
	};

public:
	vk::ShaderModule tryFindShaderOnDisk(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args = {});
	vk::ShaderModule tryFindShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args = {});

	// Utility function that compiles a shader using glslangValidator
	// Useful for live shader reloading/switching
	// Adds default include paths (src/shaders/{., include})
	// Returns nullopt on failure. glslPath should be given relative to "src/",
	// so e.g. be just "particles/particles.comp"
	vk::ShaderModule loadShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args = {});

	// TODO
	// Returns a timestamp value representing the time the given glsl
	// shader file or any of its includes were changed for the last time.
	// Note that this will read the shader (and all its includes) to find
	// included files.
	// - shader: should be given relative to TKN_BASE_DIR/src, e.g.
	//   'particles/particle.comp'.
	// - extraInclude: extra include directories, must be absolute paths.
	//   By default, will check for the include files in 'tkn/shaders/include'
	//   and the base path of the shader.
	// When an include file couldn't be found, returns nullopt.
	std::optional<std::uint64_t> shaderOrIncludesLastChanged(
		nytl::StringParam shader, nytl::Span<const char*> extraIncludes = {});

private:
	struct CompiledShader {
		// The last loaded version of this Shader
		// Might be null.
		vpp::ShaderModule mod;
		// Timestamp of when we last compiled this shader module.
		// Only valid/relevant when mod isn't null.
		fs::file_time_type lastCompiled;
	};

	struct FileInfo {
		// Maps from the arguments to the last compiled shader versions.
		std::unordered_map<std::string, CompiledShader> modules;
		// All files (absolute paths) this file includes.
		// Only direct includes.
		std::vector<std::string> includes;
		// When includes were last checked.
		fs::file_time_type includesLastChecked {};

		// TODO(optimization)
		// Timestamp of when the file was last changed (or at least, the value
		// we read the last time we checked. Might be any value greater
		// than that now).
		// std::uint64_t lastChanged;
	};

	std::unordered_map<std::string, FileInfo> known_;
};

} // namespace tkn
