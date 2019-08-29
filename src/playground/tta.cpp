// Written for my thesis
// reads gltf files and analysis their texture sizes and how well that
// model would work with various texture packing approaches

#include <tkn/gltf.hpp>
#include <tkn/image.hpp>
#include <tkn/types.hpp>
#include <dlg/dlg.hpp>
#include <nytl/vec.hpp>
#include <nytl/span.hpp>

#include "stb_rect_pack.h"
#include "atlas.hpp"

namespace gltf = tkn::gltf;
using namespace tkn::types;

// Returns the number of mip levels in a full mip chain
unsigned mipLevels(unsigned width, unsigned height) {
	return 1 + std::floor(std::log2(std::max(width, height)));
}

// Returns the total texture space required (including full mip chain)
// for a texture with the given dimensions.
u64 totalTexSpace(unsigned width, unsigned height) {
	auto levels = mipLevels(width, height);
	u64 used = 0u;
	for(auto i = 0u; i < levels; ++i) {
		used += width * height;
		width = std::max(1u, width >> 1);
		height = std::max(1u, height >> 1);
	}

	return used;
}

// the functions below analyze the texture space needed by the different
// approaches. Only requirement is that textures don't have sizes
// over 8192, which is required for texture atlases.
// Computes the required texture space including all mip levels (since
// for atlasLUT they have to be included for correct results).

using Textures = nytl::Span<nytl::Vec2ui>;

u64 textureSpaceSimple(Textures textures) {
	u64 ret = 0;
	for(auto& tex : textures) {
		ret += totalTexSpace(tex.x, tex.y);
	}
	return ret;
}

u64 textureSpaceLayered(Textures textures) {
	// find max size
	unsigned width = 0;
	unsigned height = 0;
	for(auto& tex : textures) {
		if(tex.x > width) {
			width = tex.x;
		}
		if(tex.y > height) {
			height = tex.y;
		}
	}

	return totalTexSpace(width, height) * textures.size();
}

// We always use base atlas size of 8192x8192 here. If the textures
// don't fit in one atlas, we use multiple (as we would use multiple
// atlas texture layers). In the last layer, we only count the space
// that was used by the placing algorithm. Otherwise we would get
// 8192 * 8192 texture space needed for a single 64x64 texture
// which destroys the statistics.
// We count that used space as bounding box around the placed textures.
// For POT we additionally round that up to POT since we require
// a POT atlas size there (for mipmaps).

u64 textureSpaceAtlasPOT(Textures textures) {
	std::vector<atlas::Tex> atex;
	for(auto& tex : textures) {
		auto& at = atex.emplace_back();
		at.width = tex.x;
		at.height = tex.y;
	}

	u64 used = 0u;
	while(!atex.empty()) {
		atlas::Rect rect = {0, 0, 8192, 8192};
		atlas::fill(atex, rect, true);

		nytl::Vec2ui max {0, 0};
		auto eraser = [&](const atlas::Tex& t) {
			if(t.x + t.width > max.x) {
				max.x = t.x + t.width;
			}
			if(t.y + t.height > max.y) {
				max.y = t.y + t.height;
			}
			return t.placed;
		};

		atex.erase(std::remove_if(atex.begin(), atex.end(), eraser), atex.end());
		if(atex.empty()) { // last level
			used += totalTexSpace(atlas::npot(max.x), atlas::npot(max.y));
		} else {
			// count full atlas level
			// the space wasted here is the fault of the placement
			// algorithm/the texture size constellation
			used += totalTexSpace(rect.width, rect.height);
		}
	}

	return used;
}

// naive texture placement (using POT placement algorithm)
// assumes that all textures want their full mipmap chain
u64 textureSpaceAtlasLUTNaive(Textures textures) {
	std::vector<atlas::Tex> atex;
	for(auto& tex : textures) {
		auto levels = mipLevels(tex.x, tex.y);
		auto width = tex.x;
		auto height = tex.y;
		for(auto i = 0u; i < levels; ++i) {
			auto& at = atex.emplace_back();
			at.width = width;
			at.height = height;
			width = std::max(1u, width >> 1);
			height = std::max(1u, height >> 1);
		}
	}

	u64 used = 0u;
	while(!atex.empty()) {
		atlas::Rect rect = {0, 0, 8192, 8192};
		atlas::fill(atex, rect, true);

		nytl::Vec2ui max {0, 0};
		auto eraser = [&](const atlas::Tex& t) {
			if(t.x + t.width > max.x) {
				max.x = t.x + t.width;
			}
			if(t.y + t.height > max.y) {
				max.y = t.y + t.height;
			}
			return t.placed;
		};

		atex.erase(std::remove_if(atex.begin(), atex.end(), eraser), atex.end());

		// with this atlas method we don't have explicit mip levels (they
		// are already in the first level), so we don't need to use
		// totalTexSpace
		if(atex.empty()) { // last level
			used += max.x * max.y;
		} else {
			// count full atlas level
			// the space wasted here is the fault of the placement
			// algorithm/the texture size constellation
			used += rect.width * rect.height;
		}
	}

	return used;
}

// more advanced rect packing using stb_rect_pack (skyline algorithm)
u64 textureSpaceAtlasLUTSTB(Textures textures) {
	// TODO
}


// TODO: test different distributions
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
