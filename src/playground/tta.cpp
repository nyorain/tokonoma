// Written for my thesis
// reads gltf files and analysis their texture sizes and how well that
// model would work with various texture packing approaches

#include <tkn/gltf.hpp>
#include <tkn/image.hpp>
#include <dlg/dlg.hpp>

namespace gltf = tkn::gltf;

int main(int argc, const char** argv) {
	if(argc < 2) {
		dlg_fatal("No filepath given");
		return -1;
	}

	gltf::TinyGLTF loader;
	gltf::Model model;
	std::string err;
	std::string warn;

	auto res = loader.LoadASCIIFromFile(&model, &err, &warn, argv[1]);

	// error, warnings
	auto pos = 0u;
	auto end = warn.npos;
	while((end = warn.find_first_of('\n', pos)) != warn.npos) {
		auto d = warn.data() + pos;
		dlg_warn("  {}", std::string_view{d, end - pos});
		pos = end + 1;
	}

	pos = 0u;
	while((end = err.find_first_of('\n', pos)) != err.npos) {
		auto d = err.data() + pos;
		dlg_error("  {}", std::string_view{d, end - pos});
		pos = end + 1;
	}

	if(!res) {
		dlg_fatal(">> Failed to parse model");
		return {};
	}

	for(auto& image : model.images) {
		dlg_info("{} {}", image.width, image.height);
	}
}
