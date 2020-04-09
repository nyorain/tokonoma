#pragma once

#include <optional>
#include <vpp/fwd.hpp>
#include <nytl/stringParam.hpp>

namespace tkn {

// Utility function that compiles a shader using glslangValidator
// Useful for live shader reloading/switching
// Adds default include paths (src/shaders/{., include})
// Returns none on failure. glslPath should be given relative to "src/shaders/",
// so e.g. be just "particles.comp"
std::optional<vpp::ShaderModule> loadShader(const vpp::Device& dev,
	std::string_view glslPath, nytl::StringParam args = {});

} // namespace tkn
