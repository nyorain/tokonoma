#pragma once

#include <stage/texture.hpp>
#include <stage/types.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/vec.hpp>
#include <tinygltf.hpp>
#include <variant>

namespace doi {
namespace gltf = tinygltf;

// class Scene;

struct Material {
public:
	enum class Bit : u32 {
		normalMap = (1u << 0),
		dobuleSided = (1u << 1),
		needsTexCoord0 = (1u << 2),
		needsTexCoord1 = (1u << 3),
		blend = (1u < 4)
	};

	struct Tex {
		u32 coords;
		u32 texID;
		u32 samplerID;
	};

public:
	nytl::Vec4f albedoFac {1.f, 1.f, 1.f, 1.f};
	nytl::Vec3f emissionFac {0.f, 0.f, 0.f};
	nytl::Flags<Bit> flags {};
	float roughnessFac {1.f};
	float metalnessFac {1.f};
	float alphaCutoff {0.f};
	Tex albedo;
	Tex normals;
	Tex emission;
	Tex metalRough;
	Tex occlusion;
};

/*
class Material {
public:
	// See flags in model.frag
	static constexpr u32 flagNormalMap = 1u << 0;
	static constexpr u32 flagDoubleSided = 1u << 1;
	static constexpr u32 flagNeedsTexCoords0 = 1u << 2;
	static constexpr u32 flagNeedsTexCoords1 = 1u << 3;
	static constexpr u32 flagBlend = 1u << 4;

	struct InitData {
		// image ids
		unsigned albedo;
		unsigned emission;
		unsigned metalRoughness;
		unsigned occlusion;
		unsigned normal;
		vpp::TrDs::InitData initDs;
	};

	// see scene.glsl, directly mirrored & tightly packed
	struct BufferTex {
		u32 coords;
		u32 texID;
		u32 samplerID;
	};

	struct Buffer {
		nytl::Vec4f albedoFac {1.f, 1.f, 1.f, 1.f};
		nytl::Vec3f emissionFac {0.f, 0.f, 0.f};
		u32 flags {};
		float roughnessFac {1.f};
		float metalnessFac {1.f};
		float alphaCutoff {0.f};
		BufferTex albedo;
		BufferTex normals;
		BufferTex emission;
		BufferTex metalRough;
		BufferTex occlusion;
	};

	static vk::PushConstantRange pcr();
	static vpp::TrDsLayout createDsLayout(const vpp::Device& dev);

public:
	// default, dummy material
	Material(const vpp::TrDsLayout& dsLayout, vk::ImageView dummy,
		vk::Sampler dummySampler, nytl::Vec4f albedo = {1.f, 1.f, 1.f, 1.f},
		float roughness = 1.f, float metalness = 1.f, bool doubleSided = false,
		nytl::Vec3f emission = {0.f, 0.f, 0.f});
	Material(InitData&, const gltf::Model&, const gltf::Material&,
		const vpp::TrDsLayout&, vk::ImageView dummy, Scene& scene);
	void init(InitData&, const Scene&);

	bool needsTexCoords0() const { return buf_.flags & flagNeedsTexCoords0; }
	bool needsTexCoords1() const { return buf_.flags & flagNeedsTexCoords1; }
	bool blend() const { return (buf_.flags & flagBlend); }
	const auto& buf() const { return buf_; }

protected:
	Buffer buf_;
};
*/

} // namespace doi
