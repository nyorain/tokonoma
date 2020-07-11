#pragma once

#include <optional>
#include <vpp/fwd.hpp>
#include <vpp/shader.hpp>
#include <nytl/stringParam.hpp>

#include <vector>
#include <unordered_map>
#include <array>
#include <shared_mutex>
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
	std::string_view glslPath, nytl::StringParam args = {},
	fs::path spvOutput = "live.spv");

// TODO: allow adding, removing, retrieving already compiled modules as well
// TODO: maybe also cache included files per file in the cache, to avoid
//   actually reading shader files as often as possible?
// TODO: fix vpp::Device used for ShaderCache, eliminate parameters
// from load/find?
class ShaderCache {
public:
	// TODO: replace this with some platform-specific shader cache dir
	static constexpr auto cacheDir = "shadercache/";

	// NOTE: even though the instance is thread safe, compiling the
	// same shader (with the same args) from multiple threads isn't
	// serializable and might deliver unexpected results (e.g. the shader
	// being compiled in both threads).
	// TODO: move owneship to app? not sure what is cleaner.
	// App has to clear it atm before it destroy the vulkan device.
	static ShaderCache& instance();
	static fs::path cachePathForShader(std::string_view glslPath,
		std::string_view params);

	using FsTimePoint = fs::file_time_type;
	using FsDuration = FsTimePoint::duration;
	using FsClock = FsTimePoint::clock;

public:
	std::vector<std::string> includePaths = {
		TKN_BASE_DIR "/src/",
		TKN_BASE_DIR "/src/shaders/include/",
	};

public:
	// Tries to find an already compiled/cached version of the given
	// shader, compiled with the given args. Will search the in-memory
	// cache of shader modules as well as (if not found in memory) the
	// cache dir for a compiled spv shader and load that.
	// If any shader file (the shader itself or any of its includes) are
	// newer than a found cache, aborts.
	vk::ShaderModule find(const vpp::Device& dev,
		std::string_view glslPath, const std::string& args,
		nytl::Span<const char* const> extraIncludePaths = {});

	// Utility function that compiles a shader using glslangValidator
	// Useful for live shader reloading/switching
	// Adds default include paths (src/shaders/{., include})
	// Returns nullopt on failure. glslPath should be given relative to "src/",
	// so e.g. be just "particles/particles.comp"
	vk::ShaderModule load(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args = {},
		nytl::Span<const char* const> extraIncludePaths = {});

	void clear();

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
		fs::file_time_type includesLastChecked {FsTimePoint::min()};

		// TODO(optimization)
		// Timestamp of when the file was last changed (or at least, the value
		// we read the last time we checked. Might be any value greater
		// than that now).
		// std::uint64_t lastChanged;
	};

	std::unordered_map<std::string, FileInfo> known_;
	std::shared_mutex mutex_;
};

} // namespace tkn
