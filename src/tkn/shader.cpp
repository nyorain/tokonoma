#include <tkn/shader.hpp>
#include <dlg/dlg.hpp>
#include <vpp/shader.hpp>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace tkn {

// TODO: don't use std::system for this. Instead use a proper
// subprocess abstraction. This allows to:
// - better specify arguments and environment
// - properly redirect output if needed
// - get more information about exit status? not about this
// - not block until it terminates. Async and parallel shader compilation
//   (without starting multiple thread that only wait for std::system
//   to complete, that's insane honestly) would be pretty neat
// https://github.com/benman64/subprocess looks quite promising.

// TODO: add tracking to automatically reload shaders when they change.
//   https://github.com/emcrisostomo/fswatch looks like a good cross-platform
//   solution. Not sure if here is the right place for this, though.
//   Combinding it with ShaderCache sounds like a good idea though.

std::vector<u32> readFile32(std::string_view filename, bool binary = true) {
	auto openmode = std::ios::ate;
	if(binary) {
		openmode |= std::ios::binary;
	}

	std::ifstream ifs(std::string{filename}, openmode);
	ifs.exceptions(std::ostream::failbit | std::ostream::badbit);

	auto size = ifs.tellg();
	if(size % 4) {
		throw std::runtime_error("readFile32: file length not multiple of 4");
	}

	ifs.seekg(0, std::ios::beg);

	std::vector<u32> buffer(size / 4);
	auto data = reinterpret_cast<char*>(buffer.data());
	ifs.read(data, size);

	return buffer;
}


std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args) {
	return compileShader(dev, glslPath, args);
}

// TODO: allow absolute paths.
std::optional<vpp::ShaderModule> compileShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args, fs::path spvOutput,
		std::vector<u32>* outSpv) {
	std::string cmd = "glslangValidator -V -o ";
	cmd += spvOutput;

	// include dirs
	cmd += " -I";
	cmd += TKN_BASE_DIR;
	cmd += "/src/shaders/include";

	// input
	auto fullPath = std::string(TKN_BASE_DIR);
	fullPath += "/src/";
	fullPath += glslPath;

	cmd += " ";
	cmd += fullPath;

	if(!args.empty()) {
		cmd += " ";
		cmd += args;
	}

	dlg_debug(cmd);

	// clearly mark glslang output
	struct dlg_style style {};
	style.style = dlg_text_style_bold;
	style.fg = dlg_color_magenta;
	style.bg = dlg_color_none;
	dlg_styled_fprintf(stderr, style, ">>> Start glslang <<<%s\n", dlg_reset_sequence);
	int ret = std::system(cmd.c_str());
	fflush(stdout);
	fflush(stderr);
	dlg_styled_fprintf(stderr, style, ">>> End glslang <<<%s\n", dlg_reset_sequence);

#ifdef TKN_LINUX
	if(WEXITSTATUS(ret) != 0) { // only working for posix
#else
	if(ret != 0) {
#endif
		dlg_error("Failed to compile shader {}", fullPath);
		return {};
	}

	auto data = readFile32(spvOutput.c_str());
	auto mod = vpp::ShaderModule(dev, data);
	if(outSpv) {
		*outSpv = std::move(data);
	}

	return mod;
}

// ShaderCache
ShaderCache& ShaderCache::instance(const vpp::Device& dev) {
	static ShaderCache shaderCache(dev);
	return shaderCache;
}

vk::ShaderModule ShaderCache::find(
		std::string_view shader, const std::string& args,
		nytl::Span<const char* const> extraIncludePaths,
		std::vector<std::string>* outIncluded,
		std::vector<u32>* outSpv) {
	auto shaderPath = fs::path(shader);
	auto shaderStr = std::string(shader);

	// Check if shader is known
	FsTimePoint compiledTime;
	fs::path compiledPath;
	bool foundInCache = false;

	{
		auto sharedLock = std::shared_lock(mutex_);
		auto knownIt = known_.find(shaderStr);
		if(knownIt != known_.end()) {
			// Check we have already compiled this shader with the given arguments.
			auto& known = knownIt->second;
			auto compiledIt = known.modules.find(args);
			if(compiledIt != known.modules.end()) {
				auto& compiled = compiledIt->second;
				compiledTime = compiled.lastCompiled;
				foundInCache = true;
			}
		}

		if(!foundInCache) {
			auto spvPath = cachePathForShader(shader, args);
			if(!fs::exists(spvPath)) {
				return {};
			}

			compiledTime = fs::last_write_time(spvPath);
			compiledPath = std::move(spvPath);
		}
	}

	if(!checkIncludes(shaderStr, &compiledTime, extraIncludePaths, outIncluded)) {
		return {};
	}

	if(foundInCache) {
		auto sharedLock = std::shared_lock(mutex_);
		auto knownIt = known_.find(shaderStr);
		if(knownIt != known_.end()) {
			// Check we have already compiled this shader with the given arguments.
			auto& known = knownIt->second;
			auto compiledIt = known.modules.find(args);
			if(compiledIt != known.modules.end()) {
				auto& compiled = compiledIt->second;
				if(outSpv) {
					*outSpv = compiled.spv;
				}

				return compiled.mod;
			}
		}

		// This case is weird, we checked the same above.
		dlg_warn("Shader cache module was removed while rechecking");
		return {};
	}

	auto spvPath = cachePathForShader(shader, args);
	if(!fs::exists(spvPath)) {
		// This case is weird. We checked the same above.
		dlg_warn("Shader cache file was removed while rechecking");
		return {};
	}

	std::vector<u32> data;
	try {
		data = readFile32(compiledPath.c_str());
	} catch(const std::exception& err) {
		dlg_warn("Error reading shader cache file from disk at '{}': {}",
			compiledPath, err.what());
		return {};
	}

	if(outSpv) {
		*outSpv = data;
	}

	auto newMod = vpp::ShaderModule{device(), data};

	auto lockGuard = std::lock_guard(mutex_);
	auto knownIt = known_.find(shaderStr);
	dlg_assert(knownIt != known_.end());
	auto& compiled = knownIt->second.modules[args];
	compiled.mod = std::move(newMod);
	compiled.lastCompiled = compiledTime;
	compiled.spv = std::move(data);

	return compiled.mod;
}

vk::ShaderModule ShaderCache::load(
		std::string_view shader, nytl::StringParam args,
		nytl::Span<const char* const> extraIncludePaths,
		std::vector<std::string>* outIncluded,
		std::vector<u32>* outSpv) {
	auto argsStr = std::string(args);
	auto mod = find(shader, argsStr, extraIncludePaths, outIncluded, outSpv);
	if(mod) {
		dlg_info("Loading '{}' (args: '{}') from cache", shader, argsStr);
		return mod;
	}

	for(auto& inc : extraIncludePaths) {
		argsStr += " ";
		argsStr += "-I";
		argsStr += inc;
	}

	dlg_info("Couldn't find '{}' (args: '{}') in cache, compiling it", shader, argsStr);
	auto spvPath = cachePathForShader(shader, argsStr);

	if(outIncluded) {
		// needed since 'find' might have written into it (even if not
		// succesful).
		outIncluded->clear();
		checkIncludes(shader, nullptr, extraIncludePaths, outIncluded);
	}

	std::vector<u32> spv;

	// Make sure to store (in memory and on disk) the time *before* compilation
	// as last write time. This way, if the shader is modified during
	// compilation, the cache will be immediately out of date (even though
	// we return a version without the new change here). This guarantees
	// that we (later on) will never return a module that doesn't match
	// the current code.
	auto preCompileTime = FsClock::now();
	auto optMod = tkn::compileShader(device(), shader, argsStr, spvPath, &spv);
	if(!optMod) {
		return {};
	}

	if(outSpv) {
		*outSpv = spv;
	}

	vk::ShaderModule ret;
	{
		auto lockGuard = std::lock_guard(mutex_);
		auto& knownEntry = known_[std::string(shader)].modules[std::string(argsStr)];
		knownEntry.mod = std::move(*optMod);
		knownEntry.lastCompiled = preCompileTime;
		knownEntry.spv = std::move(spv);
		ret = knownEntry.mod;
	}

	fs::last_write_time(spvPath, preCompileTime);
	return ret;
}

void ShaderCache::clear() {
	auto lockGuard = std::lock_guard(mutex_);
	known_.clear();
}

fs::path ShaderCache::cachePathForShader(std::string_view glslPath,
		std::string_view args) {
	auto spvName = std::string(glslPath);
	for(auto& c : spvName) {
		if(c == '/' || c == '\\') {
			c = '.';
		}
	}

	if(!args.empty()) {
		auto argsStr = std::string(args);
		for(auto& c : argsStr) {
			if(!std::isalnum(c)) {
				c = '.';
			}
		}

		spvName += ".";
		spvName += argsStr;
	}

	spvName += ".spv";
	return cacheDir / fs::path(spvName);
}

bool ShaderCache::checkIncludes(std::string_view shaderStr,
		FsTimePoint* compare,
		nytl::Span<const char* const> extraIncludePaths,
		std::vector<std::string>* outIncluded) {
	auto shaderPath = fs::path(shaderStr);

	// recheck includes, recursively (via a worklist).
	// TODO: might be a better idea to resolve includes already
	// when they are added to worklist.
	struct Include {
		std::string file;
		fs::path fromDir;
	};
	std::vector<Include> worklist = {{std::string(shaderStr), shaderPath.parent_path()}};
	std::unordered_set<std::string> done;

	while(!worklist.empty()) {
		auto [work, includedFromDir] = worklist.back();
		worklist.pop_back();

		auto workPath = resolve(work, includedFromDir, extraIncludePaths);
		if(workPath.empty()) {
			dlg_error("Could not resolve shader file {}", workPath);
			return false;
		}

		auto workPathStr = workPath.string();
		if(!done.insert(workPathStr).second) {
			// Already checked this header
			continue;
		}

		if(outIncluded) {
			outIncluded->push_back(workPathStr);
		}

		auto lastChanged = fs::last_write_time(workPath);
		if(compare && lastChanged > *compare) {
			dlg_info("Can't use {} from cache since {} is out of date",
				shaderStr, workPath);
			return false;
		}

		auto parentPath = workPath.parent_path();

		{
			auto sharedLock = std::shared_lock(mutex_);
			if(auto knownIt = known_.find(work); knownIt != known_.end()) {
				auto& known = knownIt->second;
				if(known.includesLastChecked > lastChanged) {

					// in this case, the currently handled file was not
					// changed since we last updated its includes.
					// But one of the include files might have changed,
					// we have to add it to the worklist.
					worklist.reserve(worklist.size() + known.includes.size());
					for(auto& inc : known.includes) {
						worklist.push_back({inc, parentPath});
					}

					// if(outIncluded) {
					// 	outIncluded->insert(outIncluded->end(),
					// 		known.includes.begin(), known.includes.end());
					// }
					continue;
				}
			}
		}

		// Recheck includes for the currnet work file.
		dlg_info("rechecking includes of {}", work);
		std::vector<std::string> newIncludes;
		auto preCheckTime = FsClock::now();

		std::ifstream in(workPath, std::ios_base::in);
		if(!in.is_open()) {
			dlg_error("Can't open {}", shaderStr);
			return false;
		}

		// TODO: allow all include forms, with whitespace and stuff
		auto incStr = std::string_view("#include \"");
		for(std::string line; std::getline(in, line);) {
			auto lineView = std::string_view(line);
			if(lineView.substr(0, incStr.size()) != incStr) {
				continue;
			}

			lineView = lineView.substr(incStr.size());
			auto delim = lineView.find('"');
			if(delim == lineView.npos) {
				dlg_error("unterminated quote in include in {}", shaderStr);
				return false;
			}

			// At this point, we have found an include file.
			auto incFile = lineView.substr(0, delim);
			dlg_info("include: {} -> {}", work, incFile);
			worklist.push_back({std::string(incFile), parentPath});
			newIncludes.push_back(std::string(incFile));
		}

		// if(outIncluded) {
		// 	outIncluded->insert(outIncluded->end(),
		// 		newIncludes.begin(), newIncludes.end());
		// }

		// If the file didn't exist, we insert it.
		auto lockGuard = std::lock_guard(mutex_);
		auto& known = known_[work];
		known.includesLastChecked = preCheckTime;
		known.includes = std::move(newIncludes);
	}

	return true;
}

fs::path ShaderCache::resolve(std::string_view shader,
		fs::path includedFromDir,
		nytl::Span<const char* const> extraIncludePaths) {
	auto shaderPath = fs::path(shader);
	if(shaderPath.is_absolute()) {
		if(!fs::exists(shaderPath)) {
			dlg_debug("resolve: Absolute shader file {} does not exist", shaderPath);
			return {};
		}

		return shaderPath;
	}

	// check own directory
	auto absPath = includedFromDir / shaderPath;
	if(fs::exists(absPath)) {
		return absPath;
	}

	// check default include paths
	for(auto& incPath : includePaths) {
		auto absPath = fs::path(incPath) / shaderPath;
		if(fs::exists(absPath)) {
			return absPath;
		}
	}

	// check custom include paths
	for(auto& incPath : extraIncludePaths) {
		auto absPath = fs::path(incPath) / shaderPath;
		if(fs::exists(absPath)) {
			return absPath;
		}
	}

	dlg_debug("Could not resolve shader file {}", shaderPath);
	return {};
}

} // namespace tkn
