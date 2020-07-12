#include <tkn/shader.hpp>
#include <dlg/dlg.hpp>
#include <vpp/shader.hpp>
#include <filesystem>
#include <fstream>

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

std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args) {
	return compileShader(dev, glslPath, args);
}

// TODO: allow absolute paths.
std::optional<vpp::ShaderModule> compileShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args, fs::path spvOutput) {
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

	return {{dev, spvOutput.c_str()}};
}

// ShaderCache
ShaderCache& ShaderCache::instance() {
	static ShaderCache shaderCache;
	return shaderCache;
}

vk::ShaderModule ShaderCache::find(const vpp::Device& dev,
		std::string_view shader, const std::string& args,
		nytl::Span<const char* const> extraIncludePaths) {
	auto shaderPath = fs::path(shader);
	auto shaderStr = std::string(shader);

	// Check if shader is known
	vk::ShaderModule compiledMod {};
	FsTimePoint compiledTime;
	fs::path compiledPath;

	{
		auto sharedLock = std::shared_lock(mutex_);
		auto knownIt = known_.find(shaderStr);
		if(knownIt != known_.end()) {
			// Check we have already compiled this shader with the given arguments.
			auto& known = knownIt->second;
			auto compiledIt = known.modules.find(args);
			if(compiledIt != known.modules.end()) {
				auto& compiled = compiledIt->second;
				compiledMod = compiled.mod;
				compiledTime = compiled.lastCompiled;
			}
		} else {
			auto spvPath = cachePathForShader(shader, args);
			if(!fs::exists(spvPath)) {
				return {};
			}

			compiledTime = fs::last_write_time(spvPath);
			compiledPath = std::move(spvPath);
		}

		sharedLock.unlock();
	}

	// recheck includes, recursively (via a worklist).
	// TODO: might be a better idea to resolve includes already
	// when they are added to worklist.
	struct Include {
		std::string file;
		fs::path fromDir;
	};
	std::vector<Include> worklist = {{shaderStr, shaderPath.parent_path()}};
	while(!worklist.empty()) {
		auto [work, includedFromDir] = worklist.back();
		worklist.pop_back();

		auto workPath = fs::path(work);
		if(workPath.is_relative()) {
			bool found = false;

			// check own directory
			auto absPath = includedFromDir / work;
			if(fs::exists(absPath)) {
				workPath = absPath;
				found = true;
			}

			// check default include paths
			if(!found) {
				for(auto& incPath : includePaths) {
					auto absPath = fs::path(incPath) / workPath;
					if(fs::exists(absPath)) {
						workPath = absPath;
						found = true;
						break;
					}
				}
			}

			if(!found) {
				// check custom include paths
				for(auto& incPath : extraIncludePaths) {
					auto absPath = fs::path(incPath) / workPath;
					if(fs::exists(absPath)) {
						workPath = absPath;
						found = true;
						break;
					}
				}
			}

			if(!found) {
				dlg_error("Could not find include file {}", workPath);
				return {};
			}
		} else if(!fs::exists(workPath)) {
			dlg_error("Absolute include {} does not exist", workPath);
			return {};
		}

		auto lastChanged = fs::last_write_time(workPath);
		if(lastChanged > compiledTime) {
			dlg_info("Can't use {} from cache since {} is out of date",
				shader, workPath);
			return {};
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
			dlg_error("Can't open {}", shader);
			return {};
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
				dlg_error("unterminated quote in include in {}", shader);
				return {};
			}

			// At this point, we have found an include file.
			auto incFile = lineView.substr(0, delim);
			dlg_info("include: {} -> {}", work, incFile);
			worklist.push_back({std::string(incFile), parentPath});
			newIncludes.push_back(std::string(incFile));
		}

		// If the file didn't exist, we insert it.
		auto lockGuard = std::lock_guard(mutex_);
		auto& known = known_[work];
		known.includesLastChecked = preCheckTime;
		known.includes = std::move(newIncludes);
	}

	// We know that the compiled mod (whether on disk or already loaded)
	// is still up-to-date and can be used.
	if(compiledMod) {
		return compiledMod;
	}

	// Load from disk.
	auto lockGuard = std::lock_guard(mutex_);
	auto knownIt = known_.find(shaderStr);
	dlg_assert(knownIt != known_.end());
	auto& compiled = knownIt->second.modules[args];
	compiled.mod = {dev, compiledPath.c_str()};
	compiled.lastCompiled = compiledTime;

	return compiled.mod;
}

vk::ShaderModule ShaderCache::load(const vpp::Device& dev,
		std::string_view shader, nytl::StringParam args,
		nytl::Span<const char* const> extraIncludePaths) {
	auto argsStr = std::string(args);
	for(auto& inc : extraIncludePaths) {
		argsStr += " ";
		argsStr += "-I";
		argsStr += inc;
	}

	auto mod = find(dev, shader, argsStr);
	if(mod) {
		dlg_info("Loading '{}' (args: '{}') from cache", shader, argsStr);
		return mod;
	}

	dlg_info("Couldn't find '{}' (args: '{}') in cache, compiling it", shader, argsStr);
	auto spvPath = cachePathForShader(shader, argsStr);

	// Make sure to store (in memory and on disk) the time *before* compilation
	// as last write time. This way, if the shader is modified during
	// compilation, the cache will be immediately out of date (even though
	// we return a version without the new change here). This guarantees
	// that we (later on) will never return a module that doesn't match
	// the current code.
	auto preCompileTime = FsClock::now();
	auto optMod = tkn::compileShader(dev, shader, argsStr, spvPath);
	if(!optMod) {
		return {};
	}

	vk::ShaderModule ret;
	{
		auto lockGuard = std::lock_guard(mutex_);
		auto& knownEntry = known_[std::string(shader)].modules[std::string(argsStr)];
		knownEntry.mod = std::move(*optMod);
		knownEntry.lastCompiled = preCompileTime;
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

} // namespace tkn
