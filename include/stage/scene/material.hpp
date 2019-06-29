#pragma once

#include <stage/types.hpp>
#include <nytl/flags.hpp>
#include <nytl/vec.hpp>

namespace doi {

/// Standard layout, tightly packed, directly corresponds to gpu
/// representation. See scene.glsl
struct Material {
public:
	enum class Bit : u32 {
		normalMap = (1u << 0),
		doubleSided = (1u << 1),
		needsTexCoord0 = (1u << 2),
		needsTexCoord1 = (1u << 3),
		blend = (1u << 4)
	};

	struct Tex {
		u32 coords {0};
		u32 texID {0}; // 0: dummy texture
		u32 samplerID {0}; // 0: default sampler
	};

public:
	nytl::Vec4f albedoFac {1.f, 1.f, 1.f, 1.f};
	nytl::Vec3f emissionFac {0.f, 0.f, 0.f};
	nytl::Flags<Bit> flags {};
	float roughnessFac {1.f};
	float metalnessFac {1.f};
	float alphaCutoff {-1.f};
	Tex albedo {};
	Tex normals {};
	Tex emission {};
	Tex metalRough {};
	Tex occlusion {};
	nytl::Vec2f _padding{};

public: // utility
	bool needsTexCoord0() const { return flags & Bit::needsTexCoord0; }
	bool needsTexCoord1() const { return flags & Bit::needsTexCoord1; }
	bool doubleSided() const { return flags & Bit::doubleSided; }
	bool blend() const { return flags & Bit::blend; }
	bool normalMapped() const { return flags & Bit::normalMap; }
};

static_assert(sizeof(Material) % 4 == 0);

} // namespace doi
