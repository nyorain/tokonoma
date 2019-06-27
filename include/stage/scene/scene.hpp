#pragma once

#include <stage/scene/material.hpp>
#include <stage/texture.hpp>
#include <stage/defer.hpp>
#include <stage/bits.hpp>
#include <stage/types.hpp>
#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/nonCopyable.hpp>
#include <vpp/fwd.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <tinygltf.hpp>
#include <vector>

// TODO: support basic AABB culling, always use indirect commands
//   could do it on the gpu, even more efficiently with khr_draw_indirect_count
// TODO: dont require multidraw support, see gbuf.vert
// TODO: support instanced drawing, detect it from gltf structure
//   we can't rely on gl_DrawID then, we should probably pass
//   matrices (and with that model/material id) per instance.
//   but then we require the drawIndirectFirstInstance vulkan features
// TODO: sorting primitives by how they are layed out in the vertex
//   buffers (when there are no other creteria)? could improve cache locality

namespace doi {
namespace gltf = tinygltf;

struct SceneRenderInfo {
	// const vpp::TrDsLayout& materialDsLayout;
	// const vpp::TrDsLayout& primitiveDsLayout;
	vk::ImageView dummyTex; // 1 pixel rgba white
	float samplerAnisotropy;

	// whether the respective vulkan features are enabled
	bool drawIndirectFirstInstance {};
	bool multiDrawIndirect {};
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
	static constexpr auto imageCount = 96u;
	static constexpr auto samplerCount = 8u;
	using Index = u32; // for indices
	using ModelID = u32;

	struct InitData {
		std::vector<Texture::InitData> images;

		vpp::TrDs::InitData initDs;
		vpp::TrDs::InitData initBlendDs;

		vpp::SubBuffer::InitData initModels;
		vpp::SubBuffer::InitData initMaterials;
		vpp::SubBuffer::InitData initModelIDs;
		vpp::SubBuffer::InitData initCmds;
		vpp::SubBuffer::InitData initBlendModelIDs;
		vpp::SubBuffer::InitData initBlendCmds;

		vpp::SubBuffer::InitData initIndices;
		vpp::SubBuffer::InitData initVertices;

		vpp::SubBuffer stage;
		vpp::SubBuffer::InitData initStage;

		// tmp accum
		unsigned indexCount {};
		unsigned vertexCount {};
		unsigned tc0Count {};
		unsigned tc1Count {};
	};

	struct Instance {
		nytl::Mat4f matrix;
		u32 materialID;
		u32 modelID; // just for picking, not related to Primitive
	};

	struct Primitive {
		unsigned indexCount; // number of indices to draw
		unsigned vertexCount; // total number of vertices
		unsigned firstIndex; // first index in scenes index buffer
		unsigned vertexOffset; // first vertex in scenes vertex buffer

		// bounds in model space
		nytl::Vec3f min;
		nytl::Vec3f max;

		std::vector<u32> indices;
		std::vector<nytl::Vec3f> positions;
		std::vector<nytl::Vec3f> normals;
		std::vector<nytl::Vec2f> texCoords0;
		std::vector<nytl::Vec2f> texCoords1;
		std::vector<Instance> instances;
	};

	static const vk::PipelineVertexInputStateCreateInfo& vertexInfo();

public:
	Scene() = default;
	void create(InitData&, const WorkBatcher&, nytl::StringParam path,
		const gltf::Model&, const gltf::Scene&, nytl::Mat4f matrix,
		const SceneRenderInfo&);
	void init(InitData&, const WorkBatcher&, vk::ImageView dummyTex);
	void createImage(unsigned id, bool srgb);

	void updateDevice(nytl::Mat4f proj);
	void render(vk::CommandBuffer, vk::PipelineLayout, bool blend) const;

	auto& primitives() { return primitives_; }
	auto& materials() { return materials_; }
	auto& images() { return images_; }
	auto& samplers() { return samplers_; }

	auto& primitives() const { return primitives_; }
	auto& materials() const { return materials_; }
	auto& images() const { return images_; }
	auto& samplers() const { return samplers_; }
	auto& defaultSampler() const { return defaultSampler_; }
	auto& dsLayout() const { return dsLayout_; }

	nytl::Vec3f min() const { return min_; }
	nytl::Vec3f max() const { return max_; }
	const vpp::Device& device() const { return defaultSampler_.device(); }

protected:
	void loadNode(InitData&, const WorkBatcher&, const gltf::Model&,
		const gltf::Node&, const SceneRenderInfo&, nytl::Mat4f matrix);
	void loadMaterial(InitData&, const WorkBatcher&, const gltf::Model&,
		const gltf::Material&, const SceneRenderInfo&);
	void loadPrimitive(InitData&, const WorkBatcher&, const gltf::Model&,
		const gltf::Primitive&, nytl::Mat4f matrix);

	bool multiDrawIndirect_ {};
	vpp::Sampler defaultSampler_;
	std::vector<Sampler> samplers_;
	std::vector<SceneImage> images_;
	std::vector<Material> materials_;
	std::vector<Primitive> primitives_;
	unsigned defaultMaterialID_ {};
	unsigned instanceID_ {};

	nytl::Vec3f min_;
	nytl::Vec3f max_;

	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::TrDs blendDs_;

	vpp::SubBuffer instanceBuf_; // matrices, material link
	vpp::SubBuffer materialsBuf_;

	unsigned opaqueCount_ {}; // number of opaque primitives
	vpp::SubBuffer cmds_; // opqque commands
	vpp::SubBuffer modelIDs_;

	unsigned blendCount_ {}; // number of blend primitives
	vpp::SubBuffer blendCmds_; // transparent commands
	vpp::SubBuffer blendModelIDs_;

	vpp::SubBuffer indices_;
	vpp::SubBuffer vertices_; // order: tc1, tc0, position + normal
	unsigned tc0Offset_; // where tc0 starts in vertices_
	unsigned posOffset_; // where positions starts in vertices_
	unsigned normalOffset_; // where normals start in vertices_

	vpp::Semaphore uploadSemaphore_;
	vpp::CommandBuffer uploadCb_;
};

// Tries to parse the given string as path or filename of a gltf/gltb file
// and parse it. On success, also returns the base path as second parameter
std::tuple<std::optional<gltf::Model>, std::string> loadGltf(nytl::StringParam);

} // namespace doi
