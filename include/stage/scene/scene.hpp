#pragma once

#include <stage/scene/material.hpp>
#include <stage/scene/primitive.hpp>
#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/nonCopyable.hpp>
#include <vpp/fwd.hpp>
#include <tinygltf.hpp>
#include <vector>

namespace doi {
namespace gltf = tinygltf;

struct SceneRenderInfo {
	const vpp::TrDsLayout& materialDsLayout;
	const vpp::TrDsLayout& primitiveDsLayout;
	vk::PipelineLayout pipeLayout;
	vk::ImageView dummyTex; // 1 pixel rgba white
};

struct SceneImage {
	vpp::ViewableImage image;
	bool srgb;
};

// TODO: make movable. Requires to remove reference from
// primitive to material
class Scene : public nytl::NonMovable {
public:
	struct Sampler {
		vk::Sampler sampler;
		vk::SamplerAddressMode addressMode;
		// TODO: other gltf-relevant parameters
	};

public:
	Scene() = default;
	Scene(vpp::Device&, nytl::StringParam path, const gltf::Model&,
		const gltf::Scene&, nytl::Mat4f matrix,
		const SceneRenderInfo&);

	void render(vk::CommandBuffer, vk::PipelineLayout) const;

	auto& primitives() { return primitives_; }
	auto& materials() { return materials_; }
	auto& images() { return images_; }
	auto& primitives() const { return primitives_; }
	auto& materials() const { return materials_; }
	auto& images() const { return images_; }

protected:
	void loadNode(vpp::Device&, const gltf::Model&, const gltf::Node&,
		const SceneRenderInfo&, nytl::Mat4f matrix);

	std::vector<SceneImage> images_;
	std::vector<Material> materials_;
	std::vector<Primitive> primitives_;

	// TODO: use (in materials)
	// std::vector<Sampler> samplers_;
};

// Tries to parse the given string as path or filename of a gltf/gltb file
// and parse a scene from it.
std::unique_ptr<Scene> loadGltf(nytl::StringParam at, vpp::Device& dev,
	nytl::Mat4f matrix, const SceneRenderInfo&);

} // namespace doi
