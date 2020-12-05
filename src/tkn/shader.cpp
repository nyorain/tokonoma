#undef DLG_DEFAULT_TAGS
#define DLG_DEFAULT_TAGS "tkn", "tkn/shader"

#include <tkn/shader.hpp>
#include <tkn/util.hpp>
#include <tkn/bits.hpp>
#include <dlg/dlg.hpp>
#include <vpp/shader.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/util/file.hpp>
#include <vkpp/enums.hpp>
#include <vkpp/functions.hpp>
#include <nytl/scope.hpp>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <unordered_set>
#include <sha1.hpp>

#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <SPIRV/Logger.h>

namespace tkn {

inline std::string readFilePath(const fs::path& path) {
	auto openmode = std::ios::ate;
	std::ifstream ifs(path, openmode);
	ifs.exceptions(std::ostream::failbit | std::ostream::badbit);

	auto size = ifs.tellg();
	ifs.seekg(0, std::ios::beg);

	std::string buffer;
	buffer.resize(size);
	auto data = reinterpret_cast<char*>(buffer.data());
	ifs.read(data, size);

	return buffer;
}

std::vector<u32> readFilePath32(const fs::path& path, bool binary = true) {
	auto openmode = std::ios::ate;
	if(binary) {
		openmode |= std::ios::binary;
	}

	std::ifstream ifs(path, openmode);
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
	cmd += spvOutput.u8string();

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

	auto data = readFilePath32(spvOutput);
	auto mod = vpp::ShaderModule(dev, data);
	if(outSpv) {
		*outSpv = std::move(data);
	}

	return mod;
}

// new api, using glslang directly as library
bool initGlslang() {
	bool ret = (ShInitialize()) || glslang::InitializeProcess();
	if(!ret) {
		dlg_error("Failed to initialize glslang");
	}

	return ret;
}

// Default include class for normal include convention of search backward
// through the stack of active include paths (for nested includes).
// Can be overridden to customize.
class DirStackFileIncluder : public glslang::TShader::Includer {
public:
    DirStackFileIncluder() : externalLocalDirectoryCount(0) { }

    virtual IncludeResult* includeLocal(const char* headerName,
            const char* includerName, size_t inclusionDepth) override {
        return readLocalPath(headerName, includerName, (int)inclusionDepth);
    }

    virtual IncludeResult* includeSystem(const char* headerName,
            const char*, size_t ) override {
        return readSystemPath(headerName);
    }

    // Externally set directories. E.g., from a command-line -I<dir>.
    //  - Most-recently pushed are checked first.
    //  - All these are checked after the parse-time stack of local directories
    //    is checked.
    //  - This only applies to the "local" form of #include.
    //  - Makes its own copy of the path.
    virtual void pushLocalDirs(nytl::Span<const char*> dirs) {
        directoryStack.insert(directoryStack.end(), dirs.begin(), dirs.end());
        externalLocalDirectoryCount += dirs.size();
    }

    virtual void releaseInclude(IncludeResult* result) override {
        if (result != nullptr) {
            delete [] static_cast<char*>(result->userData);
            delete result;
        }
    }

protected:
    std::vector<std::string> directoryStack;
    int externalLocalDirectoryCount;

    // Search for a valid "local" path based on combining the stack of include
    // directories and the nominal name of the header.
    virtual IncludeResult* readLocalPath(const char* headerName,
			const char* includerName, int depth) {
        // Discard popped include directories, and
        // initialize when at parse-time first level.
        directoryStack.resize(depth + externalLocalDirectoryCount);
        if (depth == 1)
            directoryStack.back() = getDirectory(includerName);

        // Find a directory that works, using a reverse search of the include stack.
        for (auto it = directoryStack.rbegin(); it != directoryStack.rend(); ++it) {
            std::string path = *it + '/' + headerName;
            std::replace(path.begin(), path.end(), '\\', '/');
            std::ifstream file(path, std::ios_base::binary | std::ios_base::ate);
            if (file) {
                directoryStack.push_back(getDirectory(path));
                return newIncludeResult(path, file, (int)file.tellg());
            }
        }

        return nullptr;
    }

    // Search for a valid <system> path.
    // Not implemented yet; returning nullptr signals failure to find.
    virtual IncludeResult* readSystemPath(const char*) const {
        return nullptr;
    }

    // Do actual reading of the file, filling in a new include result.
    virtual IncludeResult* newIncludeResult(const std::string& path,
			std::ifstream& file, int length) const {
        char* content = new char[length];
        file.seekg(0, file.beg);
        file.read(content, length);
        return new IncludeResult(path, content, length, content);
    }

    // If no path markers, return current working directory.
    // Otherwise, strip file name and return path leading up to it.
    virtual std::string getDirectory(const std::string path) const {
        size_t last = path.find_last_of("/\\");
        return last == std::string::npos ? "." : path.substr(0, last);
    }
};

// NOTE: we could fill this with limits from the vulkan device. And
// completely disable legacy stuff such as MaxLights. The layers already
// catch this but limit violations are something we might want to
// report even in a production environment (where layers are disabled),
// e.g. it would be really useful to include them in crash reports.
const TBuiltInResource defaultTBuiltInResource = {
    /* .MaxLights = */ 32,
    /* .MaxClipPlanes = */ 6,
    /* .MaxTextureUnits = */ 32,
    /* .MaxTextureCoords = */ 32,
    /* .MaxVertexAttribs = */ 64,
    /* .MaxVertexUniformComponents = */ 4096,
    /* .MaxVaryingFloats = */ 64,
    /* .MaxVertexTextureImageUnits = */ 32,
    /* .MaxCombinedTextureImageUnits = */ 80,
    /* .MaxTextureImageUnits = */ 32,
    /* .MaxFragmentUniformComponents = */ 4096,
    /* .MaxDrawBuffers = */ 32,
    /* .MaxVertexUniformVectors = */ 128,
    /* .MaxVaryingVectors = */ 8,
    /* .MaxFragmentUniformVectors = */ 16,
    /* .MaxVertexOutputVectors = */ 16,
    /* .MaxFragmentInputVectors = */ 15,
    /* .MinProgramTexelOffset = */ -8,
    /* .MaxProgramTexelOffset = */ 7,
    /* .MaxClipDistances = */ 8,
    /* .MaxComputeWorkGroupCountX = */ 65535,
    /* .MaxComputeWorkGroupCountY = */ 65535,
    /* .MaxComputeWorkGroupCountZ = */ 65535,
    /* .MaxComputeWorkGroupSizeX = */ 1024,
    /* .MaxComputeWorkGroupSizeY = */ 1024,
    /* .MaxComputeWorkGroupSizeZ = */ 64,
    /* .MaxComputeUniformComponents = */ 1024,
    /* .MaxComputeTextureImageUnits = */ 16,
    /* .MaxComputeImageUniforms = */ 8,
    /* .MaxComputeAtomicCounters = */ 8,
    /* .MaxComputeAtomicCounterBuffers = */ 1,
    /* .MaxVaryingComponents = */ 60,
    /* .MaxVertexOutputComponents = */ 64,
    /* .MaxGeometryInputComponents = */ 64,
    /* .MaxGeometryOutputComponents = */ 128,
    /* .MaxFragmentInputComponents = */ 128,
    /* .MaxImageUnits = */ 8,
    /* .MaxCombinedImageUnitsAndFragmentOutputs = */ 8,
    /* .MaxCombinedShaderOutputResources = */ 8,
    /* .MaxImageSamples = */ 0,
    /* .MaxVertexImageUniforms = */ 0,
    /* .MaxTessControlImageUniforms = */ 0,
    /* .MaxTessEvaluationImageUniforms = */ 0,
    /* .MaxGeometryImageUniforms = */ 0,
    /* .MaxFragmentImageUniforms = */ 8,
    /* .MaxCombinedImageUniforms = */ 8,
    /* .MaxGeometryTextureImageUnits = */ 16,
    /* .MaxGeometryOutputVertices = */ 256,
    /* .MaxGeometryTotalOutputComponents = */ 1024,
    /* .MaxGeometryUniformComponents = */ 1024,
    /* .MaxGeometryVaryingComponents = */ 64,
    /* .MaxTessControlInputComponents = */ 128,
    /* .MaxTessControlOutputComponents = */ 128,
    /* .MaxTessControlTextureImageUnits = */ 16,
    /* .MaxTessControlUniformComponents = */ 1024,
    /* .MaxTessControlTotalOutputComponents = */ 4096,
    /* .MaxTessEvaluationInputComponents = */ 128,
    /* .MaxTessEvaluationOutputComponents = */ 128,
    /* .MaxTessEvaluationTextureImageUnits = */ 16,
    /* .MaxTessEvaluationUniformComponents = */ 1024,
    /* .MaxTessPatchComponents = */ 120,
    /* .MaxPatchVertices = */ 32,
    /* .MaxTessGenLevel = */ 64,
    /* .MaxViewports = */ 16,
    /* .MaxVertexAtomicCounters = */ 0,
    /* .MaxTessControlAtomicCounters = */ 0,
    /* .MaxTessEvaluationAtomicCounters = */ 0,
    /* .MaxGeometryAtomicCounters = */ 0,
    /* .MaxFragmentAtomicCounters = */ 8,
    /* .MaxCombinedAtomicCounters = */ 8,
    /* .MaxAtomicCounterBindings = */ 1,
    /* .MaxVertexAtomicCounterBuffers = */ 0,
    /* .MaxTessControlAtomicCounterBuffers = */ 0,
    /* .MaxTessEvaluationAtomicCounterBuffers = */ 0,
    /* .MaxGeometryAtomicCounterBuffers = */ 0,
    /* .MaxFragmentAtomicCounterBuffers = */ 1,
    /* .MaxCombinedAtomicCounterBuffers = */ 1,
    /* .MaxAtomicCounterBufferSize = */ 16384,
    /* .MaxTransformFeedbackBuffers = */ 4,
    /* .MaxTransformFeedbackInterleavedComponents = */ 64,
    /* .MaxCullDistances = */ 8,
    /* .MaxCombinedClipAndCullDistances = */ 8,
    /* .MaxSamples = */ 4,
    /* .maxMeshOutputVerticesNV = */ 256,
    /* .maxMeshOutputPrimitivesNV = */ 512,
    /* .maxMeshWorkGroupSizeX_NV = */ 32,
    /* .maxMeshWorkGroupSizeY_NV = */ 1,
    /* .maxMeshWorkGroupSizeZ_NV = */ 1,
    /* .maxTaskWorkGroupSizeX_NV = */ 32,
    /* .maxTaskWorkGroupSizeY_NV = */ 1,
    /* .maxTaskWorkGroupSizeZ_NV = */ 1,
    /* .maxMeshViewCountNV = */ 4,
    /* .maxDualSourceDrawBuffersEXT = */ 1,

    /* .limits = */ {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    }};

std::vector<u32> compileShader(
		nytl::StringParam glslSource,
		EShLanguage shlang,
		nytl::StringParam sourcePath,
		nytl::StringParam cpreamble,
		nytl::Span<const char*> includeDirs) {
	static bool glslangInit = initGlslang();
	if(!glslangInit) {
		return {};
	}

	using namespace glslang;
	auto str = glslSource.c_str();
	auto strName = sourcePath.c_str();
	auto strLen = int(glslSource.length());

	auto shader = TShader(shlang);
	shader.setStringsWithLengthsAndNames(&str, &strLen, &strName, 1);
	shader.setEnvInput(EShSourceGlsl, shlang, EShClientVulkan, 100);
	shader.setEnvClient(EShClientVulkan, EShTargetVulkan_1_0);
	shader.setEnvTarget(EShTargetSpv, EShTargetSpv_1_0);

	std::string preamble = "#extension GL_GOOGLE_include_directive : require\n";
	preamble += cpreamble;
	preamble += '\n';
	shader.setPreamble(preamble.c_str());

    DirStackFileIncluder includer;
	includer.pushLocalDirs(includeDirs);

    auto messages = EShMessages(EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);
	if(!shader.parse(&defaultTBuiltInResource, 450, ENoProfile,
			false, false, messages, includer)) {
		dlg_error("Compiling shader '{}' failed: {}", sourcePath, shader.getInfoLog());
		return {};
	}

	auto log = shader.getInfoLog();
	if(log && *log != '\0') {
		dlg_info("Shader '{}', compilation info: {}", sourcePath, log);
	}

	log = shader.getInfoDebugLog();
	if(log && *log != '\0') {
		dlg_debug("Shader '{}', debug compilation info: {}", sourcePath, log);
	}

	auto intermed = shader.getIntermediate();
	dlg_assert(intermed);

    auto logger = spv::SpvBuildLogger{};
	auto options = SpvOptions{};
	options.generateDebugInfo = true;
	// options.disableOptimizer = false;

	TProgram program;
	program.addShader(&shader);
	if(!program.link(messages)) {
		dlg_error("Linking shader '{}' failed: {}", sourcePath, program.getInfoLog());
		return {};
	}

	log = program.getInfoLog();
	if(log && *log != '\0') {
		dlg_info("Shader '{}', link info: {}", sourcePath, log);
	}

	log = program.getInfoDebugLog();
	if(log && *log != '\0') {
		dlg_debug("Shader '{}', debug link info: {}", sourcePath, log);
	}

	program.mapIO();

	// NOTE: we could use this instead of spirv-reflect
	// program.buildReflection();

	// This assumption is made by glslang spirv.
	// If it ever causes a problem, should be changed in glslang.
	static_assert(sizeof(unsigned) == sizeof(u32));
	std::vector<unsigned> spirv;
	GlslangToSpv(*program.getIntermediate(shlang), spirv, &logger, &options);

	auto spvLog = logger.getAllMessages();
	if(!spvLog.empty()) {
		dlg_info("SPV logger: {}", spvLog);
	}

	if constexpr(std::is_same_v<u32, unsigned>) {
		return spirv;
	} else {
		dlg_warn("Need to copy spirv since glslang assumes u32 == unsigned");
		return {std::vector<u32>(spirv.begin(), spirv.end())};
	}
}

bool deduceShaderStage(nytl::StringParam glslPath, EShLanguage& outStage) {
	if(hasSuffixCI(glslPath, ".frag")) {
		// outStage = vk::ShaderStageBits::fragment;
		outStage = EShLangFragment;
		return true;
	} else if(hasSuffixCI(glslPath, ".vert")) {
		// outStage = vk::ShaderStageBits::vertex;
		outStage = EShLangVertex;
		return true;
	} else if(hasSuffixCI(glslPath, ".comp")) {
		// outStage = vk::ShaderStageBits::compute;
		outStage = EShLangCompute;
		return true;
	}

	return false;
}

std::vector<u32> compileShader(const fs::path& glslPath,
		nytl::StringParam preamble,
		nytl::Span<const char*> includeDirs) {
	EShLanguage lang;
	if(!deduceShaderStage(glslPath.u8string(), lang)) {
		dlg_warn("Can't deduce shader type of {}", glslPath);
		return {};
	}

	std::string source = readFilePath(glslPath);
	return compileShader(source, lang, glslPath.u8string(), preamble, includeDirs);
}

// ShaderCache
ShaderCache& ShaderCache::instance(const vpp::Device& dev) {
	static ShaderCache shaderCache(dev);
	dlg_assert(&shaderCache.device() == &dev);
	return shaderCache;
}

bool ShaderCache::reparse(std::string_view source,
		std::vector<std::string>& outIncluded, Hash& outHash) {
	Sha1 sha;

	for(auto rest = source; !rest.empty(); source = rest) {
		auto nline = source.find("\n");
		std::string_view line;
		std::tie(line, rest) = splitIf(source, nline);

		line = skipWhitespace(line);
		const auto commentStr = std::string_view("//");
		if(line.substr(0, commentStr.size()) == commentStr) {
			continue;
		}

		// TODO: better check for includes
		sha.add(line.data(), line.size());
		const auto incStr = std::string_view("#include \"");
		if(line.substr(0, incStr.size()) != incStr) {
			continue;
		}

		line = line.substr(incStr.size());
		auto delim = line.find('"');
		if(delim == line.npos) {
			dlg_error("unterminated quote in include");
			return false;
		}

		// At this point, we have found an include file.
		auto incFile = line.substr(0, delim);
		outIncluded.push_back(std::string(incFile));
	}

	sha.finalize();
	sha.print_hex(outHash.data(), true);
	return true;
}

bool ShaderCache::buildCurrentHash(fs::path shaderPath, Hash& outHash) {
	Sha1 sha;
	std::unordered_set<std::string> done;
	std::vector<fs::path> worklist {shaderPath};

	while(!worklist.empty()) {
		auto item = worklist.back();
		auto itemString = item.string();
		worklist.pop_back();
		done.insert(item.string());

		{
			auto sharedLock = std::shared_lock(mutex_);
			auto knownIt = known_.find(itemString);
			if(knownIt != known_.end()) {
				auto& known = knownIt->second;
				auto sourceLastWritten = fs::last_write_time(item);
				if(sourceLastWritten < known.lastParsed) {
					auto& hash = known.hash;
					sha.add(hash.data(), hash.size());
					worklist.insert(worklist.end(),
						known.includes.begin(), known.includes.end());
					continue;
				}
			}
		}

		// parsed file is out-of-date or not known at all.
		// reparse it
		std::vector<std::string> included;
		Hash hash;
		auto timeBeforeParsed = FsClock::now();
		auto source = readFilePath(itemString);
		dlg_debug("reparsing {}", item);
		if(!reparse(source, included, hash)) {
			// parsing failed for some reason
			return false;
		}

		std::vector<fs::path> includedPaths;
		includedPaths.reserve(included.size());
		for(auto& incStr : included) {
			auto path = resolve(incStr, item.parent_path());
			if(path.empty()) {
				// could not resolve shader include
				dlg_warn("Could not resolve shader {}", incStr);
				return false;
			}

			dlg_debug(" {} -> {}", item, path);
			includedPaths.push_back(path);
		}

		worklist.insert(worklist.end(), includedPaths.begin(),
			includedPaths.end());
		sha.add(hash.data(), hash.size());

		{
			auto lock = std::lock_guard(mutex_);
			auto& known = known_[itemString];
			known.includes = std::move(includedPaths);
			known.hash = hash;
			known.lastParsed = timeBeforeParsed;
		}
	}

	sha.finalize();
	sha.print_hex(outHash.data(), true);
	return true;
}

ShaderCache::CompiledShaderView ShaderCache::find(
		const std::string& fullPathStr, const Hash& hash) {

	{
		auto sharedLock = std::shared_lock(mutex_);
		auto knownIt = known_.find(fullPathStr);
		dlg_assertm(knownIt != known_.end(),
			"There must be an entry for this shader, we generated a hash!");

		if(knownIt != known_.end()) {
			auto& known = knownIt->second;
			auto shaderIt = known.modules.find(hash);
			if(shaderIt != known.modules.end()) {
				return {shaderIt->second.mod, shaderIt->second.spv};
			}
		}
	}

	// check if it exists on disk
	auto spvPath = cachePathForShader(hash);
	if(!fs::exists(spvPath)) {
		// potential disk cache does not exist
		return {};
	}

	std::vector<u32> spv;
	try {
		spv = readFilePath32(spvPath);
	} catch(const std::exception& err) {
		dlg_warn("Error reading shader cache file from disk at {}: {}",
			spvPath, err.what());
		return {};
	}

	auto newMod = vpp::ShaderModule{device(), spv};

	auto lockGuard = std::lock_guard(mutex_);
	auto knownIt = known_.find(fullPathStr);
	dlg_assertm(knownIt != known_.end(),
		"There must be an entry for this shader, we generated a hash!");

	auto compiled = CompiledShader{std::move(newMod), std::move(spv)};
	auto [it, emplaced] = knownIt->second.modules.try_emplace(hash, std::move(compiled));
	dlg_assertm(emplaced, "Could not insert compiled shader module since it "
		"already existed. Either this is a sha1 collision or another thread "
		"compiled and inserted it at the same time as this one");

	return {it->second.mod, it->second.spv};
}

ShaderCache::CompiledShaderView ShaderCache::load(
		std::string_view shaderPath, nytl::StringParam preamble) {

	// rough idea of shader loading
	// - retrieve hash of requested shader
	//   check for shader and all includes if file hash is still up to date
	//   if not: reparse the file
	//   while doing so, build up hash. Make sure to include the preamble in the hash
	// - check if compiled version of hash is present
	//   if so: return it
	//   otherwise: compile shader from scratch

	auto fullPath = resolve(shaderPath);
	if(fullPath.empty()) {
		dlg_error("Can't load {}, could not resolve path", shaderPath);
		return {};
	}

	auto fullPathStr = fullPath.string();

	// check if it's a plain spirv shader
	if(!fullPath.has_extension() || fullPath.extension() == ".spv") {
		dlg_debug("Interpreting shader {} as spirv", fullPath);
		dlg_assertm(preamble.empty(), "Can't use preamble for precompiled shaders");

		auto lastWritten = fs::last_write_time(fullPath);

		{
			auto lock = std::shared_lock(mutex_);
			auto knownIt = known_.find(fullPathStr);
			if(knownIt != known_.end()) {
				auto& known = knownIt->second;
				if(known.lastParsed > lastWritten) {
					auto modIt = known.modules.find(known.hash);
					if(modIt != known.modules.end()) {
						return {modIt->second.mod, modIt->second.spv};
					}
				}
			}
		}

		// not found/not up to date. We have to reload it.
		auto timeBeforeRead = FsClock::now();
		auto spv = readFilePath32(fullPath);

		Sha1 sha;
		sha.add(spv.data(), spv.size() * sizeof(spv[0]));
		sha.finalize();

		auto lock = std::lock_guard(mutex_);
		auto& known = known_[fullPathStr];
		known.includes.clear();
		known.lastParsed = timeBeforeRead;
		sha.print_hex(known.hash.data());

		auto newMod = vpp::ShaderModule(device(), spv);
		auto compiled = CompiledShader{std::move(newMod), std::move(spv)};
		auto [it, emplaced] = known.modules.try_emplace(known.hash, std::move(compiled));
		dlg_assertm(emplaced, "Could not insert compiled shader module since it "
			"already existed. Either this is a sha1 collision or another thread "
			"compiled and inserted it at the same time as this one");

		return {it->second.mod, it->second.spv};
	}

	// check if shader is already known
	Hash sourceHash;
	if(!buildCurrentHash(fullPath, sourceHash)) {
		// Some error during parsing the files
		return {};
	}

	Sha1 sha;
	sha.add(sourceHash.data(), sourceHash.size());
	sha.add(preamble.data(), preamble.size());
	sha.finalize();

	Hash hash;
	sha.print_hex(hash.data(), true);

	auto mod = find(fullPathStr, hash);
	if(mod.mod) {
		dlg_debug("Loading {} (hash: '{}') from cache", fullPath, hash.data());
		return mod;
	}

	dlg_debug("recompiling {}", fullPath);

	// Make sure to store (in memory and on disk) the time *before* compilation
	// as last write time. This way, if the shader is modified during
	// compilation, the cache will be immediately out of date (even though
	// we return a version without the new change here). This guarantees
	// that we (later on) will never return a module that doesn't match
	// the current code.
	auto preCompileTime = FsClock::now();
	auto spv = tkn::compileShader(fullPathStr, preamble, includePaths);
	if(spv.empty()) {
		return {};
	}

	auto spvPath = cachePathForShader(hash);
	auto spvPathParent = spvPath.parent_path();
	if(!fs::exists(spvPathParent)) {
		fs::create_directories(spvPathParent);
	}

	vpp::writeFile(spvPath.u8string(), tkn::bytes(spv), true);
	fs::last_write_time(spvPath, preCompileTime);

	auto lockGuard = std::lock_guard(mutex_);
	auto& known = known_[fullPathStr];

	auto newMod = vpp::ShaderModule(device(), spv);
	auto compiled = CompiledShader{std::move(newMod), std::move(spv)};
	auto [it, emplaced] = known.modules.try_emplace(hash, std::move(compiled));
	dlg_assertm(emplaced, "Could not insert compiled shader module since it "
		"already existed. Either this is a sha1 collision or another thread "
		"compiled and inserted it at the same time as this one");

	return {it->second.mod, it->second.spv};
}

void ShaderCache::clear() {
	auto lockGuard = std::lock_guard(mutex_);
	known_.clear();
}

fs::path ShaderCache::cachePathForShader(const Hash& hash) {
	auto spvName = std::string(hash.data(), hash.size() - 1);
	spvName += ".spv";
	return cacheDir / fs::path(spvName);
}

fs::path ShaderCache::resolve(std::string_view shader,
		const fs::path& includedFromDir) {
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

	dlg_debug("Could not resolve shader file {}", shaderPath);
	return {};
}

std::vector<std::string> ShaderCache::resolveIncludes(std::string_view shader) {
	auto shaderPath = resolve(shader);
	if(shaderPath.empty()) {
		dlg_warn("Failed to resolve shader '{}'", shader);
		return {};
	}

	return includes(shaderPath);
}

std::vector<std::string> ShaderCache::includes(const fs::path& shaderPath) {
	auto lock = std::shared_lock(mutex_);
	std::vector<std::string> ret = {};
	std::vector<fs::path> worklist = {std::move(shaderPath)};
	std::unordered_set<std::string> seen;
	while(!worklist.empty()) {
		auto item = worklist.back();
		worklist.pop_back();
		seen.insert(item.u8string());

		auto known = known_.find(item.string());
		if(known == known_.end()) {
			dlg_warn("Could not find {} in shader cache", item);
			return {};
		}

		auto& included = known->second.includes;
		worklist.insert(worklist.end(), included.begin(), included.end());

		for(auto& inc : included) {
			ret.push_back(inc.u8string());
		}
	}

	return ret;
}

vk::ShaderModule ShaderCache::insertSpv(const std::string& fullPathStr,
		FsTimePoint lastParsed, const Hash& hash, CompiledShader compiled) {
	auto lock = std::shared_lock(mutex_);
	auto& known = known_[fullPathStr];
	known.lastParsed = lastParsed;
	known.hash = hash;

	auto [it, emplaced] = known.modules.try_emplace(hash, std::move(compiled));
	dlg_assertm(emplaced, "Compiled modules already existed in shader cache");
	return it->second.mod;
}

// PipelineCache
static PipelineCache* gPipelineCache {};
std::mutex& pipelineCacheMutex() {
	static std::mutex ret;
	return ret;
}

vk::PipelineCache PipelineCache::instance(const vpp::Device& dev) {
	static PipelineCache cache(dev);

	auto lock = std::lock_guard(pipelineCacheMutex());
	dlg_assert(gPipelineCache);
	dlg_assert(gPipelineCache == &cache);
	if(!gPipelineCache) {
		dlg_error("Pipeline cache was already finished");
		return {};
	}

	auto it = cache.caches_.try_emplace(std::this_thread::get_id(), dev).first;

	static thread_local auto threadGuard = nytl::ScopeGuard{[]{
		auto lock = std::lock_guard(pipelineCacheMutex());
		auto* pc = gPipelineCache;
		if(pc) {
			auto it = pc->caches_.find(std::this_thread::get_id());
			if(it == pc->caches_.end()) {
				dlg_warn("Could not find thread is caches");
				return;
			}

			vk::mergePipelineCaches(pc->device_, pc->mainCache_, {{it->second.vkHandle()}});
			pc->caches_.erase(it);
		}
	}};

	return it->second;
}

void PipelineCache::finishInstance() {
	auto lock = std::lock_guard(pipelineCacheMutex());
	if(gPipelineCache) {
		gPipelineCache->finish();
	}
}

PipelineCache::PipelineCache(const vpp::Device& dev) : device_(dev), mainCache_(dev, path) {
	auto lock = std::lock_guard(pipelineCacheMutex());
	gPipelineCache = this;
}

PipelineCache::~PipelineCache() {
	auto lock = std::lock_guard(pipelineCacheMutex());
	if(gPipelineCache == this) {
		finish();
	}
}

void PipelineCache::finish() {
	gPipelineCache = nullptr;
	std::vector<vk::PipelineCache> srcCaches;
	for(auto& cache : caches_) {
		srcCaches.push_back(cache.second);
	}

	if(!srcCaches.empty()) {
		vk::mergePipelineCaches(device_, mainCache_, srcCaches);
	}
	vpp::save(mainCache_, path);

	caches_.clear();
	mainCache_ = {};
}

} // namespace tkn
