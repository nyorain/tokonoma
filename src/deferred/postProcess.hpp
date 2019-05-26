// WIP

#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

class PostProcessPass {
public:
	static constexpr u32 tonemapClamp = 0u;
	static constexpr u32 tonemapReinhard = 1u;
	static constexpr u32 tonemapUncharted2 = 2u;
	static constexpr u32 tonemapACES = 3u;
	static constexpr u32 tonemapHeijlRichard = 4u;

	static constexpr u32 flagFXAA = 1 << 1;

public:
	struct {
		u32 flags {flagFXAA};
		u32 tonemap {tonemapReinhard};
		float exposure {1.f};
	} params;
};
