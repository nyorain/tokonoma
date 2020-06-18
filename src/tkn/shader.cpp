#include <tkn/shader.hpp>
#include <dlg/dlg.hpp>
#include <vpp/shader.hpp>

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
	dlg_styled_fprintf(stderr, style, ">>> Start glslang <<<%s\n",
		dlg_reset_sequence);
	int ret = std::system(cmd.c_str());
	fflush(stdout);
	fflush(stderr);
	dlg_styled_fprintf(stderr, style, ">>> End glslang <<<%s\n",
		dlg_reset_sequence);

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

} // namespace tkn
