#pragma once

#include <vector>
#include <cmath>

namespace atlas {

struct Rect {
	unsigned x, y, width, height;
};

struct Tex {
	unsigned x {}, y {};
	unsigned width, height; // must be powers of 2
	bool placed {};
	long id; // for identifying the texture later on
};

// Returns the next power of two for the given number.
// Might be the number itself.
unsigned npot(unsigned x) {
	return std::pow(2, std::ceil(std::log2(x)));
}

// Selects the best fitting tex in worklist, places it in the
// given space and returns it. Returns nullptr if no tex can be
// fit into the given space.
Tex* place(std::vector<Tex>& textures, const Rect& space, bool align) {
	Tex* best = nullptr;
	for(auto& tex : textures) {
		if(tex.placed) {
			continue;
		}

		// make sure the texture fits into the given space
		bool better = tex.width <= space.width && tex.height <= space.height;
		if(align) {
			// make sure the starting point is a multiple of the texture size
			// in each dimension. Required to make sure that on higher levels,
			// the texture will always be pixel-aligned (e.g. a texture with size
			// 2048 starting at position 2 starts at pixel 0.5 in the
			// third level which makes correct addressing with linear interpolation
			// more difficult).
			auto m = std::min(tex.width, tex.height);
			better &= (space.x % m == 0 && space.y % m == 0);
		}
		// We optimize for height and then for width, i.e. want to fit largest
		// texture first. If we already found a texture that fits the criteria,
		// only choose the current one if it's bettter.
		better &= !best || tex.height > best->height ||
			(tex.height == best->height && tex.width > best->width);

		if(better) {
			best = &tex;
		}
	}

	if(best) {
		best->x = space.x;
		best->y = space.y;
		best->placed = true;
	}

	return best;
}

// Tries to place all the given textures into the given rect.
// align: whether to align texture placements with their size as
//   is needed for POT atlases.
void fill(std::vector<Tex>& textures, const Rect& rect, bool align) {
	std::vector<Rect> worklist; // stack
	worklist.push_back(rect);

	while(!worklist.empty()) {
		auto rect = worklist.back();
		worklist.pop_back();

		auto pnext = place(textures, rect, align);
		if(!pnext) {
			continue;
		}

		auto& next = *pnext;
		if(next.height < rect.height) { // permanently split row (bottom)
			Rect subrow {
				rect.x,
				rect.y + next.height,
				rect.width,
				rect.height - next.height,
			};

			worklist.push_back(subrow);
		}

		if(next.width < rect.width) { // next column in current (top) row
			Rect nextcol {
				rect.x + next.width,
				rect.y,
				rect.width - next.width,
				next.height,
			};

			worklist.push_back(nextcol);
		}
	}
}

} // namespace atlas
