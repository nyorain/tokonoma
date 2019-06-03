#pragma once

#include <stage/scene/material.hpp>
#include <stage/scene/primitive.hpp>
#include <stage/texture.hpp>
#include <stage/defer.hpp>
#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/nonCopyable.hpp>
#include <vpp/fwd.hpp>
#include <tinygltf.hpp>
#include <vector>

// TODO: support basic AABB culling, always use indirect commands

namespace doi {
namespace gltf = tinygltf;

struct SceneRenderInfo {
	const vpp::TrDsLayout& materialDsLayout;
	const vpp::TrDsLayout& primitiveDsLayout;
	vk::ImageView dummyTex; // 1 pixel rgba white
	float samplerAnisotropy;
};

struct SceneImage {
	doi::Texture image;
	bool srgb;
	bool needed {};
};

struct SamplerInfo {
	vk::Filter minFilter;
	vk::Filter magFilter;
	vk::SamplerAddressMode addressModeU;
	vk::SamplerAddressMode addressModeV;
	vk::SamplerMipmapMode mipmapMode {};

	SamplerInfo() = default;
	SamplerInfo(const gltf::Sampler& sampler);
};

bool operator==(const SamplerInfo& a, const SamplerInfo& b);
bool operator!=(const SamplerInfo& a, const SamplerInfo& b);

struct Sampler {
	vpp::Sampler sampler;
	SamplerInfo info;

	Sampler() = default;
	Sampler(const vpp::Device&, const gltf::Sampler&, float maxAnisotropy);
};

class Scene {
public:
	struct InitData {
		std::vector<Texture::InitData> images;
		std::vector<Material::InitData> materials;
		std::vector<Primitive::InitData> primitives;
		vpp::SubBuffer::InitData initBlendCmds;
	};

public:
	Scene() = default;
	Scene(InitData&, const WorkBatcher&, nytl::StringParam path,
		const gltf::Model&, const gltf::Scene&, nytl::Mat4f matrix,
		const SceneRenderInfo&);

	void init(InitData&, const WorkBatcher&);
	void createImage(unsigned id, bool srgb);

	void updateDevice(nytl::Vec3f viewPos);

	void render(vk::CommandBuffer, vk::PipelineLayout) const;
	void renderOpaque(vk::CommandBuffer, vk::PipelineLayout) const;
	void renderBlend(vk::CommandBuffer, vk::PipelineLayout) const;

	auto& primitives() { return primitives_; }
	auto& materials() { return materials_; }
	auto& images() { return images_; }
	auto& samplers() { return samplers_; }

	auto& primitives() const { return primitives_; }
	auto& materials() const { return materials_; }
	auto& images() const { return images_; }
	auto& samplers() const { return samplers_; }
	auto& defaultSampler() const { return defaultSampler_; }

	nytl::Vec3f min() const { return min_; }
	nytl::Vec3f max() const { return max_; }

protected:
	void loadNode(InitData&, const WorkBatcher&, const gltf::Model&,
		const gltf::Node&, const SceneRenderInfo&, nytl::Mat4f matrix);

	vpp::Sampler defaultSampler_;
	std::vector<Sampler> samplers_;
	std::vector<SceneImage> images_;
	std::vector<Material> materials_;
	std::vector<Primitive> primitives_;
	unsigned defaultMaterialID_ {};

	nytl::Vec3f min_;
	nytl::Vec3f max_;

	vpp::SubBuffer blendCmds_;
};

// Tries to parse the given string as path or filename of a gltf/gltb file
// and parse it. On success, also returns the base path as second parameter
std::tuple<std::optional<gltf::Model>, std::string> loadGltf(nytl::StringParam);

} // namespace doi
