#pragma once

#include "material.hpp"
#include "primitive.hpp"
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

	void render(vk::CommandBuffer, vk::PipelineLayout);

protected:
	void loadNode(vpp::Device&, const gltf::Model&, const gltf::Node&,
		const SceneRenderInfo&, nytl::Mat4f matrix);

	std::vector<Primitive> primitives_;
	std::vector<Material> materials_;
	std::vector<vpp::ViewableImage> images_;

	// TODO: use (in materials)
	std::vector<Sampler> samplers_;
};

} // namespace doi
