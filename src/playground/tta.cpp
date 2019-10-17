// Written for my thesis
// reads gltf files and analysis their texture sizes and how well that
// model would work with various texture packing approaches

#include <tkn/gltf.hpp>
#include <tkn/image.hpp>
#include <tkn/types.hpp>
#include <dlg/dlg.hpp>
#include <nytl/vec.hpp>
#include <nytl/span.hpp>

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"
#include "atlas.hpp"

#include <random>

namespace gltf = tkn::gltf;
using namespace tkn::types;

constexpr auto atlasWidth = 2 * 8192u;
constexpr auto atlasHeight = 2 * 8192u;

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
		at.width = atlas::npot(tex.x);
		at.height = atlas::npot(tex.y);
	}

	u64 used = 0u;
	while(!atex.empty()) {
		atlas::Rect rect = {0, 0, atlasWidth, atlasHeight};
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
			// used += totalTexSpace(atlas::npot(max.x), atlas::npot(max.y));
			used += totalTexSpace(max.x, max.y);
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
	nytl::Vec2ui max {0, 0};
	auto eraser = [&](const atlas::Tex& t) {
		if(!t.placed) {
			return false;
		}

		if(t.x + t.width > max.x) {
			max.x = t.x + t.width;
		}
		if(t.y + t.height > max.y) {
			max.y = t.y + t.height;
		}
		return true;
	};

	while(!atex.empty()) {
		atlas::Rect rect = {0, 0, atlasWidth, atlasHeight};
		atlas::fill(atex, rect, false);

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
	stbrp_context ctx {};
	stbrp_node* nodes = static_cast<stbrp_node*>(std::calloc(atlasWidth, sizeof(stbrp_node)));

	std::vector<stbrp_rect> rects;
	for(auto& tex : textures) {
		auto levels = mipLevels(tex.x, tex.y);
		auto width = tex.x;
		auto height = tex.y;
		for(auto i = 0u; i < levels; ++i) {
			auto& rect = rects.emplace_back();
			rect.w = width;
			rect.h = height;
			rect.was_packed = 0;
			width = std::max(1u, width >> 1);
			height = std::max(1u, height >> 1);
		}
	}

	nytl::Vec2ui max {0, 0};
	auto eraser = [&](const stbrp_rect& r) {
		if(!r.was_packed) {
			return false;
		}

		if(r.x + r.w > max.x) {
			max.x = r.x + r.w;
		}
		if(r.y + r.h > max.y) {
			max.y = r.y + r.h;
		}
		return true;
	};

	u64 used = 0u;
	while(true) {
		stbrp_init_target(&ctx, atlasWidth, atlasHeight, nodes, atlasWidth);
		auto res = stbrp_pack_rects(&ctx, rects.data(), rects.size());

		rects.erase(std::remove_if(rects.begin(), rects.end(), eraser), rects.end());

		dlg_assert(rects.empty() == (res == 1));
		if(res == 1) { // all rects placed
			used += max.x * max.y;
			break;
		}

		used += atlasWidth * atlasHeight;
	}

	return used;
}

using PlacementAlgorithm = u64(*)(Textures);

struct {
	const char* name;
	PlacementAlgorithm algorithm;
} algorithms[] = {
	{"simple/bindless", textureSpaceSimple},
	{"layered", textureSpaceLayered},
	{"atlasPOT", textureSpaceAtlasPOT},
	{"atlasLUT (simple packing)", textureSpaceAtlasLUTNaive},
	{"atlasLUT (stb rect packing)", textureSpaceAtlasLUTSTB},
};

void printReqs(Textures textures) {
	for(auto& algorithm : algorithms) {
		auto needed = algorithm.algorithm(textures);
		auto kb = needed / 1024.f;
		auto mb = kb / 1024.f;
		dlg_info("{}: {} ({} KB; {} MB)", algorithm.name, needed, kb, mb);
	}
}

std::vector<nytl::Vec2ui> loadGltfTextures(const char* filepath) {
	gltf::TinyGLTF loader;
	gltf::Model model;
	std::string err;
	std::string warn;

	auto res = loader.LoadASCIIFromFile(&model, &err, &warn, filepath);

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

	std::vector<nytl::Vec2ui> textures;
	for(auto& image : model.images) {
		dlg_assert(image.width > 0 && image.height > 0);
		auto w = unsigned(image.width);
		auto h = unsigned(image.height);
		dlg_assertm(w <= atlasWidth && h <= atlasHeight, "{} {}", w, h);
		textures.push_back({w, h});
	}

	return textures;
}

struct Stats {
	float best {std::numeric_limits<float>::infinity()};
	float worst {0.f};
	float avg {0.f};
	unsigned count {0u};
};

void eval(std::array<Stats, 4>& stats, Textures textures) {
	auto base = textureSpaceSimple(textures);
	dlg_assert(base > 0);
	for(auto i = 0u; i < 4; ++i) {
		auto needed = algorithms[1 + i].algorithm(textures);
		auto fac = float(needed) / base;
		if(fac > stats[i].worst) {
			stats[i].worst = fac;
		}
		if(fac < stats[i].best) {
			stats[i].best = fac;
		}
		++stats[i].count;
		stats[i].avg += (fac - stats[i].avg) / float(stats[i].count);
	}
}

void printStats(const std::array<Stats, 4>& stats) {
	for(auto i = 0u; i < 4; ++i) {
		dlg_info("{}", algorithms[1 + i].name);
		dlg_info("  best: {}", stats[i].best);
		dlg_info("  worst: {}", stats[i].worst);
		dlg_info("  avg: {}", stats[i].avg);
	}
}

using Distribution = std::function<unsigned()>;
void evalDistribution(const Distribution& distr) {
	std::array<Stats, 4> stats;

	std::vector<nytl::Vec2ui> textures;
	for(auto count = 10u; count < 100; ++count) {
		dlg_info("count: {}", count);
		for(auto n = 0; n < 20; ++n) {
			// dlg_info("  n: {}", n);
			textures.clear();
			for(auto i = 0u; i < count; ++i) {
				unsigned w = distr();
				unsigned h = distr();
				textures.push_back({w, h});
			}

			eval(stats, textures);
		}
	}

	printStats(stats);
}

enum class Mode {
	gltf,
	randomPOT,
	randomUniform,
	randomLognormal,
};

int main(int, const char**) {
	Mode mode = Mode::gltf;
	if(mode == Mode::gltf) {
		auto home = std::getenv("HOME");
		dlg_assert(home);
		auto basePath = home + std::string("/code/ext/glTF-Sample-Models/2.0/");
		auto models = {
			"Sponza/glTF/Sponza.gltf",
			"Lantern/glTF-pbrSpecularGlossiness/Lantern.gltf",
			"SciFiHelmet/glTF/SciFiHelmet.gltf",
			"BoomBox/glTF-pbrSpecularGlossiness/BoomBox.gltf",
			"DamagedHelmet/glTF/DamagedHelmet.gltf",
			"FlightHelmet/glTF/FlightHelmet.gltf",
			"Monster/glTF/Monster.gltf",
			"WaterBottle/glTF-pbrSpecularGlossiness/WaterBottle.gltf",
		};

		std::array<Stats, 4> stats;
		for(auto m : models) {
			dlg_info("model: {}", m);
			auto path = basePath + m;
			auto textures = loadGltfTextures(path.c_str());
			eval(stats, textures);
		}

		printStats(stats);
		return EXIT_SUCCESS;
	}

    std::random_device rd;
    std::mt19937 gen(rd());

	if(mode == Mode::randomPOT) {
		dlg_info("distribution: uniform pot");
		std::uniform_int_distribution<unsigned> dis(0, 12);
		evalDistribution([&]{
			return unsigned(std::pow(2, dis(gen)));
		});
	} else if(mode == Mode::randomUniform) {
		dlg_info("distribution: uniform");
		std::uniform_int_distribution<unsigned> dis(1, 4000);
		evalDistribution([&]{ return dis(gen); });
	} else if(mode == Mode::randomLognormal) {
		std::lognormal_distribution<float> dis(6.f, 1.25f);
		evalDistribution([&]{
			return std::min(unsigned(dis(gen)), 4096u);
		});
	}
}
