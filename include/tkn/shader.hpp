#pragma once

#include <optional>
#include <vpp/fwd.hpp>
#include <vpp/shader.hpp>
#include <vpp/handles.hpp>
#include <nytl/stringParam.hpp>
#include <tkn/types.hpp>
#include <tkn/function.hpp>

#include <thread>
#include <vector>
#include <unordered_map>
#include <array>
#include <shared_mutex>
#include <cstring>
#include <filesystem>
namespace fs = std::filesystem;

namespace tkn {

// Renamed to compileShader. This one is just badly named.
[[deprecated("Use ShaderCache or alternatives below instead")]]
std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
	std::string_view glslPath, nytl::StringParam args = {});

// Utility function that compiles a shader using glslangValidator
// Useful for live shader reloading/switching.
// Adds default include paths (src/shaders/ include)
// Returns none on failure. glslPath should be given relative to "src/",
// so e.g. be just "particles/particles.comp"
// Prefer using ShaderCache which holds a cache of compiled shader modules
// on disk and in memory.
[[deprecated("Use ShaderCache or alternatives below instead")]]
std::optional<vpp::ShaderModule> compileShader(const vpp::Device& dev,
	std::string_view glslPath, nytl::StringParam args = {},
	fs::path spvOutput = "live.spv", std::vector<u32>* outSpv = nullptr);

std::vector<u32> compileShader(const fs::path& glslPath,
	nytl::StringParam preamble,
	nytl::Span<const char*> includeDirs);

// TODO: test and re-audit synchronization. Might have issues, mainly
//   when one module is compiled by multiple threads at the same time.
// TODO: Re-check hash *after* compilation and retry if it has changed?
//   Currently, if a file changes between hash building and compilation,
//   we will store the compiled mod under an hash not matching the
//   source we compiled. Or maybe while building the hash, already load
//   all needed files?
//   But, is that a even usecase we really want to support?
// PERF(low): maybe also cache included files (& hash) per file in the disk cache,
//   to avoid actually reading shader files as often as possible?
//   we currently have to parse them at every startup.

// Loads and compiles glsl shaders.
// Has an internal in-memory cache for compiled shader modules as well
// as a disk cache. Correctly resolves all includes and will recompile
// modules when out of date. Works purely with hashes so can cache
// multiple versions of the same path (multiple revisions or compiled
// with different preambles i.e. defines).
class ShaderCache {
public:
	// TODO: replace this with some platform-specific shader cache dir
	static constexpr auto cacheDir = "shadercache/";

	// NOTE: Does not support multiple shader caches for multiple devices.
	// TODO: move owneship to app? not sure what is cleaner.
	// App has to clear it atm before it destroys the vulkan device.
	static ShaderCache& instance(const vpp::Device& dev);

	using FsTimePoint = fs::file_time_type;
	using FsDuration = FsTimePoint::duration;
	using FsClock = FsTimePoint::clock;
	using Hash = std::array<char, 41>; // hex sha1, nullterminated

	// Hashes our internal long hash representation into a single
	// size_t, needed for unordered_map.
	struct HashHasher {
		std::size_t operator()(const Hash& hash) const {
			std::size_t ret {};
			for(auto i = 0u; i < hash.size() / 8u; ++i) {
				std::uint64_t dst;
				std::memcpy(&dst, &hash[8 * i], 8);
				ret ^= std::hash<std::uint64_t>{}(dst);
			}

			return ret;
		}
	};

	// Immutable
	struct CompiledShader {
		// The last loaded version of this Shader
		vpp::ShaderModule mod;
		// Compiled/Loaded SPIR-V bytecode.
		std::vector<u32> spv;
	};

	// We can return views of CompiledShader instances since they are
	// immutable once created
	struct CompiledShaderView {
		vk::ShaderModule mod {};
		nytl::Span<const u32> spv;
	};

	struct FileInfo {
		// Maps from the preamble to the last compiled shader versions.
		std::unordered_map<Hash, CompiledShader, HashHasher> modules;
		// All files (absolute paths) this file includes.
		// Only direct includes.
		std::vector<fs::path> includes;
		// SHA-1 base64 of source code.
		Hash hash;
		// When includes were last checked and the hash was generated.
		FsTimePoint lastParsed {FsTimePoint::min()};
	};

public:
	// TODO: synchronization.
	// Should probably be private and have its own mutex.
	// OTOH modifying it while compilations are being done is weird anyways.
	std::vector<const char*> includePaths = {
		TKN_BASE_DIR "/src/",
		TKN_BASE_DIR "/src/shaders/include/",
	};

public:
	ShaderCache(const vpp::Device& dev) : dev_(dev) {}

	// Tries to find an already compiled/cached version of the given
	// shader, compiled with the given args. Will search the in-memory
	// cache of shader modules as well as (if not found in memory) the
	// cache dir for a compiled spv shader and load that.
	// If any shader file (the shader itself or any of its includes) are
	// newer than a found cache, aborts.
	CompiledShaderView find(const std::string& fullPathStr, const Hash& hash);

	// Utility function that compiles a shader using glslangValidator
	// Useful for live shader reloading/switching
	// Adds default include paths (src/shaders/{., include})
	// Returns nullopt on failure. glslPath should be given relative to "src/",
	// so e.g. be just "particles/particles.comp"
	CompiledShaderView load(std::string_view shaderPath,
		nytl::StringParam preamble = {});

	// Inserts a manually loaded and compiled module into this cache.
	vk::ShaderModule insertSpv(const std::string& fullPathStr,
		FsTimePoint lastParsed, const Hash& hash, CompiledShader compiled);

	/// Only returns data already present, never updates anything.
	std::vector<std::string> resolveIncludes(std::string_view shaderPath);
	std::vector<std::string> includes(const fs::path& shaderPath);

	fs::path resolve(std::string_view shader,
		const fs::path& includedFromDir = {});

	const vpp::Device& device() const { return dev_; }
	void clear(); // NOTE: not threadsafe, deletes all compiled shaders

private:
	bool buildCurrentHash(fs::path shaderPath, Hash& outHash);

	static bool reparse(std::string_view sourceString,
		std::vector<std::string>& outIncluded,
		Hash& outHash);
	static fs::path cachePathForShader(const Hash& hash);

private:
	const vpp::Device& dev_;
	std::unordered_map<std::string, FileInfo> known_;
	std::shared_mutex mutex_;
};

// TODO: evaluate whether this can be merged with ThreadState
class PipelineCache {
public:
	// Returns a thread-specific pipeline cache instance.
	// NOTE: Does not support multiple caches for multiple devices.
	static vk::PipelineCache instance(const vpp::Device& dev);
	static void finishInstance();

	// TODO: as with the shader cache path, move this to platform-specific
	// global cache dir.
	static constexpr auto path = ".pipelinecache";

protected:
	PipelineCache(const vpp::Device& dev);
	~PipelineCache();
	void finish();

	struct ThreadInstance;

	const vpp::Device& device_;
	vpp::PipelineCache mainCache_;
	std::unordered_map<std::thread::id, vpp::PipelineCache> caches_;
};

} // namespace tkn
