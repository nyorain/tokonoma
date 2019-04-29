#pragma once

#include <stage/texture.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/vec.hpp>
#include <tinygltf.hpp>
#include <variant>

namespace doi {
namespace gltf = tinygltf;

struct SceneImage;
struct Sampler;

class Material {
public:
	// See flags in model.frag
	enum class Flags : std::uint32_t {
		normalMap = (1u << 0),
		doubleSided = (1u << 1),
		textured = (1u << 2),
	};

	static vk::PushConstantRange pcr();
	static vpp::TrDsLayout createDsLayout(const vpp::Device& dev);

public:
	// default, dummy material
	Material(const vpp::TrDsLayout& dsLayout, vk::ImageView dummy,
		vk::Sampler dummySampler, nytl::Vec4f albedo = {1.f, 1.f, 1.f, 1.f},
		float roughness = 1.f, float metalness = 1.f, bool doubleSided = false);
	Material(const gltf::Model& model, const gltf::Material& material,
		const vpp::TrDsLayout& layout, vk::ImageView dummy,
		vk::Sampler defaultSampler, nytl::StringParam path,
		nytl::Span<SceneImage> images, nytl::Span<const Sampler> samplers);

	// Returns true if this material has *any* texture
	bool hasTexture() const;
	void bind(vk::CommandBuffer cb, vk::PipelineLayout) const;

protected:
	void initDs(const vpp::TrDsLayout& layout);

protected:
	// maps
	nytl::Vec4f albedo_ {1.f, 1.f, 1.f, 1.f}; // baseColor
	float roughness_ {1.f};
	float metalness_ {1.f};
	float alphaCutoff_ {-1.f}; // opaque alpha mode
	nytl::Vec3f emission_ {0.f, 0.f, 0.f};

	// optional maps
	struct Tex {
		vk::ImageView view;
		vk::Sampler sampler;
	};

	Tex albedoTex_;
	Tex metalnessRoughnessTex_;
	Tex normalTex_;
	Tex occlusionTex_;
	Tex emissionTex_;

	vpp::TrDs ds_;
	nytl::Flags<Flags> flags_;
};

NYTL_FLAG_OPS(Material::Flags)

} // namespace doi
