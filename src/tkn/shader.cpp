#include <tkn/shader.hpp>
#include <dlg/dlg.hpp>
#include <vpp/shader.hpp>
#include <filesystem>
#include <fstream>

using FsTimePoint = fs::file_time_type;
using FsDuration = FsTimePoint::duration;
using FsClock = FsTimePoint::clock;

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

std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args) {
	return compileShader(dev, glslPath, args);
}

std::optional<vpp::ShaderModule> compileShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args) {
	static const auto spv = "live.frag.spv";
	std::string cmd = "glslangValidator -V -o ";
	cmd += spv;

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

	return {{dev, spv}};
}

// ShaderCache
vk::ShaderModule ShaderCache::tryFindShaderOnDisk(const vpp::Device& dev,
		std::string_view shader, nytl::StringParam args) {
	auto spvName = std::string(cacheDir) + std::string(shader);
	for(auto& c : spvName) {
		if(c == '/' || c == '\\') {
			c = '.';
		}
	}
	spvName += std::hash<std::string_view>{}(args);
	spvName += ".spv";

	auto spvPath = fs::path(spvName);
	if(!fs::exists(spvPath)) {
		return {};
	}

}

vk::ShaderModule ShaderCache::tryFindShader(const vpp::Device& dev,
		std::string_view shader, nytl::StringParam args) {
	auto shaderPath = fs::path(shader);
	auto shaderStr = std::string(shader);

	// Check if shader is known
	auto knownIt = known_.find(shaderStr);

	vk::ShaderModule compiledMod;
	FsTimePoint compiledTime;
	fs::path compiledPath;

	auto argsStr = std::string(args);
	if(knownIt != known_.end()) {
		auto& known = knownIt->second;

		// Check we have already compiled this shader with the given arguments.
		auto compiledIt = known.modules.find(argsStr);
		if(compiledIt != known.modules.end()) {
			auto& compiled = compiledIt->second;
			compiledMod = compiled.mod;
			compiledTime = compiled.lastCompiled;
		}
	} else {
		// try to find a compiled version on disk
		auto spvName = std::string(cacheDir) + std::string(shader);
		for(auto& c : spvName) {
			if(c == '/' || c == '\\') {
				c = '.';
			}
		}
		spvName += std::hash<std::string_view>{}(args);
		spvName += ".spv";

		auto spvPath = fs::path(spvName);
		if(!fs::exists(spvPath)) {
			return {};
		}

		compiledTime = fs::last_write_time(spvPath);
		compiledPath = std::move(spvPath);
	}

	// recheck includes, recursively (via a worklist).
	std::vector<std::string> worklist = {shaderStr};
	while(!worklist.empty()) {
		auto work = worklist.back();
		worklist.pop_back();

		auto workPath = fs::path(work);
		if(workPath.is_relative()) {
			bool found = true;
			for(auto& incPath : includePaths) {
				auto absPath = fs::path(incPath) / workPath;
				if(fs::exists(absPath)) {
					workPath = absPath;
					found = true;
					break;
				}
			}

			if(!found) {
				dlg_error("Could not find include file {}", workPath);
				return {};
			}
		} else if(fs::exists(workPath)) {
			dlg_error("Absolute include {} does not exist", workPath);
			return {};
		}

		auto lastChanged = fs::last_write_time(workPath);
		if(lastChanged > compiledTime) {
			return {};
		}

		// If it did not exist previously, we just insert it.
		// Maybe it's older than the compiler shader module anyways.
		auto& known = known_[work];
		if(known.includesLastChecked > lastChanged) {
			// in this case, the currently handled file was not
			// changed since we last updated its includes.
			// But one of the include files might have changed,
			// we have to add it to the worklist.
			for(auto& inc : known.includes) {
				worklist.push_back(inc);
			}

			// We don't have to update known.includesLastChecked since
			// we already know the file wasn't updated since then.
			continue;
		}

		// Recheck the included files for the current work item.
		known.includes.clear();
		std::ifstream in(work, std::ios_base::in);
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
			worklist.push_back(std::string(incFile));
		}

		known.includesLastChecked = FsClock::now();
	}

	// We know that the compiled mod (whether on disk or already loaded)
	// is still up-to-date and can be used.
	if(compiledMod) {
		return compiledMod;
	}

	// Load from disk.
	knownIt = known_.find(shaderStr);
	dlg_assert(knownIt != known_.end());
	auto& compiled = knownIt->second.modules[argsStr];
	compiled.mod = {dev, compiledPath.c_str()};
	compiled.lastCompiled = compiledTime;

	return compiled.mod;
}

vk::ShaderModule ShaderCache::loadShader(const vpp::Device& dev,
		std::string_view glslPath, nytl::StringParam args) {
	auto mod = tryFindShader(dev, glslPath, args);
	if(mod) {
		return mod;
	}

	dlg_info("Couldn't find {} (args: {}) in cache, compiling it", glslPath, args);
	auto optMod = tkn::loadShader(dev, glslPath, args);
	if(!optMod) {
		return {};
	}

	auto& knownEntry = known_[std::string(glslPath)].modules[std::string(args)];
	knownEntry.mod = std::move(*optMod);
	knownEntry.lastCompiled = FsClock::now();
	return knownEntry.mod;
}

} // namespace tkn
