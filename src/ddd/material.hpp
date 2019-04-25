#pragma once

#include <stage/texture.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <nytl/vec.hpp>
#include <tinygltf.hpp>
#include <variant>

namespace gltf = tinygltf;

// TODO: don't laod textures/images per material, maybe they are shared
// between materials. Many models combine occlusion and metal/roughness
// into one texture (which makes sense), we currently allocate two
// full textures
class Material {
public:
	static vpp::TrDsLayout createDsLayout(const vpp::Device& dev,
		vk::Sampler sampler);

public:
	Material(const vpp::Device& dev, const gltf::Model& model,
		const gltf::Material& material, const vpp::TrDsLayout& layout,
		vk::ImageView dummy, nytl::Span<const vpp::ViewableImage> images);

	// Returns true if this material has *any* texture
	bool hasTexture() const;
	void bind(vk::CommandBuffer cb, vk::PipelineLayout) const;

protected:
	// maps
	nytl::Vec4f albedo_ {1.f, 1.f, 1.f, 1.f}; // baseColor
	float roughness_ {1.f};
	float metalness_ {1.f};

	// optional maps
	vk::ImageView albedoTex_;
	vk::ImageView metalnessRoughnessTex_;
	vk::ImageView normalTex_;
	vk::ImageView occlusionTex_;

	vpp::TrDs ds_;
	bool textured_ {}; // whether at least one texture is valid
	bool normalmap_ {}; // whether normal map is valid
};

