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

class Scene;

class Material {
public:
	// See flags in model.frag
	static constexpr u32 flagNormalMap = 1u << 0;
	static constexpr u32 flagDoubleSided = 1u << 1;
	static constexpr u32 flagNeedsTexCoords0 = 1u << 2;
	static constexpr u32 flagNeedsTexCoords1 = 1u << 3;

	struct InitData {
		// image ids
		unsigned albedo;
		unsigned emission;
		unsigned metalRoughness;
		unsigned occlusion;
		unsigned normal;
		vpp::TrDs::InitData initDs;
	};

	// see scene.glsl
	struct PCR { // tighly packed for std140
		nytl::Vec4f albedo {1.f, 1.f, 1.f, 1.f};
		nytl::Vec3f emission {0.f, 0.f, 0.f};
		u32 flags {};
		// additional factors
		float roughness {1.f};
		float metalness {1.f};
		float alphaCutoff {0.f};
		u32 albedoCoords {};
		u32 emissionCoords {};
		u32 normalsCoords {};
		u32 metalRoughCoords {};
		u32 occlusionCoords {};
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

	bool needsTexCoords0() const { return pcr_.flags & flagNeedsTexCoords0; }
	bool needsTexCoords1() const { return pcr_.flags & flagNeedsTexCoords1; }
	void bind(vk::CommandBuffer cb, vk::PipelineLayout) const;

protected:
	void updateDs();

protected:
	PCR pcr_;

	// TODO: strictly speaking, we don't need to store the samplers after
	// the initial updates
	struct Tex {
		vk::ImageView view {};
		vk::Sampler sampler {};
	};

	Tex albedoTex_;
	Tex metalnessRoughnessTex_;
	Tex normalTex_;
	Tex occlusionTex_;
	Tex emissionTex_;

	vpp::TrDs ds_;
};

} // namespace doi
