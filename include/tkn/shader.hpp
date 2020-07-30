#pragma once

#include <optional>
#include <vpp/fwd.hpp>
#include <vpp/shader.hpp>
#include <nytl/stringParam.hpp>
#include <tkn/types.hpp>

#include <vector>
#include <unordered_map>
#include <array>
#include <shared_mutex>
#include <filesystem>
#include <cstring>
namespace fs = std::filesystem;

namespace tkn {

// Renamed to compileShader. This one is just badly named.
[[deprecated("Use ShaderCache or other overloads instead")]]
std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
	std::string_view glslPath, nytl::StringParam args = {});

// Utility function that compiles a shader using glslangValidator
// Useful for live shader reloading/switching.
// Adds default include paths (src/shaders/ include)
// Returns none on failure. glslPath should be given relative to "src/",
// so e.g. be just "particles/particles.comp"
// Prefer using ShaderCache which holds a cache of compiled shader modules
// on disk and in memory.
[[deprecated("Use ShaderCache or overloads below")]]
std::optional<vpp::ShaderModule> compileShader(const vpp::Device& dev,
	std::string_view glslPath, nytl::StringParam args = {},
	fs::path spvOutput = "live.spv", std::vector<u32>* outSpv = nullptr);

// TODO: re-add!
// std::vector<u32> compileShader(
// 	nytl::StringParam glslSource,
// 	vk::ShaderStageBits stage,
// 	nytl::StringParam sourcePath,
// 	nytl::StringParam preamble,
// 	nytl::Span<const char*> includeDirs);

std::vector<u32> compileShader(const fs::path& glslPath,
	nytl::StringParam preamble,
	nytl::Span<const char*> includeDirs);

// TODO: synchonization surely has some bugs still.
// TODO: add more fine-grained per file or per compiled module mutexes?
//   mainly for point below.
// TODO: more efficient (yet still threadsafe) returning of spv and
//   'included'. Allow callers to hold shared read lock?
// TODO: fix use of hashing
// TODO: allow adding & retrieving already compiled modules as well
// TODO: maybe also cache included files (& hash) per file in the disk cache,
//   to avoid actually reading shader files as often as possible?
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
	static ShaderCache& instance(const vpp::Device& dev);

	using FsTimePoint = fs::file_time_type;
	using FsDuration = FsTimePoint::duration;
	using FsClock = FsTimePoint::clock;
	using Hash = std::array<char, 41>; // hex sha1, nullterminated

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

	struct CompiledShader {
		// The last loaded version of this Shader
		// Might be null.
		vpp::ShaderModule mod;
		// Compiled SPIR-V.
		std::vector<u32> spv;
	};

	struct FileInfo {
		// Maps from the preamble to the last compiled shader versions.
		std::unordered_map<Hash, CompiledShader, HashHasher> modules;
		// All files (absolute paths) this file includes.
		// Only direct includes.
		std::vector<fs::path> includes;
		// SHA-1 base64 of source code and include hashes.
		Hash hash;
		// When includes were last checked and the hash was generated.
		fs::file_time_type lastParsed {FsTimePoint::min()};
	};

public:
	// TODO: synchronization.
	// Should probably be private and have its own mutex.
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
	vk::ShaderModule find(const std::string& fullPathStr, const Hash& hash,
		std::vector<u32>* outSpv = nullptr);

	// Utility function that compiles a shader using glslangValidator
	// Useful for live shader reloading/switching
	// Adds default include paths (src/shaders/{., include})
	// Returns nullopt on failure. glslPath should be given relative to "src/",
	// so e.g. be just "particles/particles.comp"
	vk::ShaderModule load(std::string_view glslPath,
		nytl::StringParam preamble = {},
		std::vector<u32>* outSpv = nullptr);

	/// Only returns data already present, never updates anything.
	std::vector<std::string> includes(std::string_view shader);
	fs::path resolve(std::string_view shader,
		const fs::path& includedFromDir = {});

	const vpp::Device& device() const { return dev_; }
	void clear(); // NOTE: not really threadsafe

private:
	bool buildCurrentHash(fs::path shaderPath, Hash& outHash);

	static bool reparse(std::string_view sourceString,
		std::vector<std::string>& outIncluded,
		Hash& outHash);
	static fs::path cachePathForShader(const Hash& hash);

	const vpp::Device& dev_;
	std::unordered_map<std::string, FileInfo> known_;
	std::shared_mutex mutex_;
};

} // namespace tkn
