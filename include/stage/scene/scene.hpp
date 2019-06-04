#pragma once

#include <stage/scene/material.hpp>
#include <stage/scene/primitive.hpp>
#include <stage/texture.hpp>
#include <stage/defer.hpp>
#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/nonCopyable.hpp>
#include <vpp/fwd.hpp>
#include <vpp/vk.hpp>
#include <tinygltf.hpp>
#include <vector>

// TODO: support basic AABB culling, always use indirect commands
//   could do it on the gpu, even more efficiently with khr_draw_indirect_count
// TODO: dont require multidraw support, see gbuf.vert
// TODO: support instanced drawing, detect it from gltf structure
//   we can't rely on gl_DrawID then, we should probably pass
//   matrices (and with that model/material id) per instance anyways

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
	static constexpr auto imageCount = 32u;
	static constexpr auto samplerCount = 8u;

	struct InitData {
		std::vector<Texture::InitData> images;

		vpp::TrDs::InitData initDs;
		vpp::SubBuffer::InitData initModels;
		vpp::SubBuffer::InitData initMaterials;
		vpp::SubBuffer::InitData initModelIDs;
		vpp::SubBuffer::InitData initCmds;
		vpp::SubBuffer::InitData initBlendModelIDs;
		vpp::SubBuffer::InitData initBlendCmds;

		vpp::SubBuffer::InitData initVert;
		vpp::SubBuffer::InitData initTexCoords0;
		vpp::SubBuffer::InitData initTexCoords1;
		std::vector<std::byte> vertData;
		std::vector<std::byte> texCoord0Data;
		std::vector<std::byte> texCoord1Data;

		vpp::SubBuffer stage;
		vpp::SubBuffer::InitData initStage;
	};

	struct Primitive {
		vk::DrawIndexedIndirectCommand cmd;
		nytl::Mat4f mat;
		unsigned material;
		unsigned id;
	};

	struct MaterialTex {
		u32 coords;
		u32 texID;
		u32 samplerID;
	};

	struct Material {
		nytl::Vec4f albedoFac {1.f, 1.f, 1.f, 1.f};
		nytl::Vec3f emissionFac {0.f, 0.f, 0.f};
		u32 flags {};
		float roughnessFac {1.f};
		float metalnessFac {1.f};
		float alphaCutoff {0.f};
		MaterialTex albedo;
		MaterialTex normals;
		MaterialTex emission;
		MaterialTex metalRough;
		MaterialTex occlusion;
	};

public:
	Scene() = default;
	Scene(InitData&, const WorkBatcher&, nytl::StringParam path,
		const gltf::Model&, const gltf::Scene&, nytl::Mat4f matrix,
		const SceneRenderInfo&);

	void init(InitData&, const WorkBatcher&, vk::ImageView dummyTex);
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

	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;

	vpp::SubBuffer modelsBuf_;
	vpp::SubBuffer materialsBuf_;

	unsigned opaqueCount_ {}; // number of opaque primitives
	vpp::SubBuffer cmds_; // opque
	vpp::SubBuffer modelIDs_;

	unsigned blendCount_ {}; // number of blend primitives
	vpp::SubBuffer blendCmds_; // transparent
	vpp::SubBuffer blendModelIDs_;

	vpp::SubBuffer indices_;
	vpp::SubBuffer vertices_; // position + normal
	vpp::SubBuffer texCoords0_;
	vpp::SubBuffer texCoords1_;

	// TODO: instances
	// for each primitive instance:
	// - transform row 0 (4 x float)
	// - transform row 1 (4 x float)
	// - transform row 2 (4 x float)
	// - modelID, matID (2 x uint TODO: check if uint supported)
	vpp::SubBuffer instanceBuf_;
};

// Tries to parse the given string as path or filename of a gltf/gltb file
// and parse it. On success, also returns the base path as second parameter
std::tuple<std::optional<gltf::Model>, std::string> loadGltf(nytl::StringParam);

} // namespace doi
