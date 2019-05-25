#pragma once

#include "pass.hpp"
#include <stage/render.hpp>
#include <stage/types.hpp>

// TODO: might make sense to give DOF its own pass (with proper impl).
// Or at least push it into the final pp stage so that at least SSR
// gets DOF applied. maybe we might get really good DOF results with
// a custom compute shader?

/// Postprocessing pass that applies light scattering, ssr, DOF and bloom.
/// Always a compute pipeline, has no targets on its own but operates
/// directly on the light target.
class CombinePass {
public:
	static constexpr u32 flagScattering = (1 << 0);
	static constexpr u32 flagSSR = (1 << 1);
	static constexpr u32 flagDOF = (1 << 2);
	static constexpr u32 flagBloom = (1 << 3);
	static constexpr u32 flagBloomDecrease = (1 << 4);

	struct InitData {
		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initUbo;
	};

public:
	struct {
		u32 flags {flagBloomDecrease};
		float dofFocus {1.f};
		float dofStrength {0.5f};
		float bloomStrength {0.25f};
	} params;

public:

protected:
};
