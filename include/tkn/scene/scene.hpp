#pragma once

#include <tkn/scene/material.hpp>
#include <tkn/texture.hpp>
#include <tkn/defer.hpp>
#include <tkn/bits.hpp>
#include <tkn/types.hpp>
#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/nonCopyable.hpp>
#include <vpp/fwd.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <tinygltf.hpp>
#include <vector>

// TODO: fix updateDs returning in upload
// TODO: dont require multidraw support, see gbuf.vert
// TODO: update min_,max_ when matrix is changed, new primitive added?
// IDEA: decouple scene and gltf loading, support .obj and other open formats
// PERF: sorting primitives by how they are layed out in the vertex
//   buffers (when there are no other creteria)? could improve cache locality
// PERF: support basic AABB culling, always use indirect commands
//   could do it on the gpu, even more efficiently with khr_draw_indirect_count

namespace tkn {
namespace gltf = tinygltf;

struct SceneRenderInfo {
	vk::ImageView dummyTex; // 1 pixel rgba white
	float samplerAnisotropy;

	// whether the respective vulkan features are enabled
	bool drawIndirectFirstInstance {};
	bool multiDrawIndirect {};
};

struct SceneImage {
	tkn::Texture image;
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

/// Manages all geometry and material buffers as well as the instances
/// that use them.
class Scene {
public:
	// NOTE: this is important for TAA but causes worse result without it
	static constexpr auto mipLodBias = -1.f;
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
		unsigned tc0Count {};
		unsigned tc1Count {};
	};

	/// Raw geometry data for a primitive.
	/// Can be rendered multiple time (by multiple instances)
	/// with different materials and transform matrices.
	struct Primitive {
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
	};

	/// Connects a Primitive to a Material and defined its transform.
	/// Instances are what is rendered in the end.
	struct Instance {
		nytl::Mat4f matrix;
		nytl::Mat4f lastMatrix;
		u32 primitiveID;
		u32 materialID;
		u32 modelID; // just for picking, not related to Primitive
	};

	static const vk::PipelineVertexInputStateCreateInfo& vertexInfo();

public:
	Scene() = default;
	void create(InitData&, const WorkBatcher&, nytl::StringParam path,
		const gltf::Model&, const gltf::Scene&, nytl::Mat4f matrix,
		const SceneRenderInfo&);
	void init(InitData&, const WorkBatcher&, vk::ImageView dummyTex);
	void createImage(unsigned id, bool srgb);

	// optionally returns semaphore that should be waited upon before
	// tknng any rendering involding the scene. In that case a
	// re-record is needed additionally.
	vk::Semaphore updateDevice(nytl::Mat4f proj);
	void render(vk::CommandBuffer, vk::PipelineLayout, bool blend) const;

	auto& primitives() { return primitives_; }
	auto& instances() { return instances_; }
	auto& materials() { return materials_; }
	auto& images() { return images_; }
	auto& samplers() { return samplers_; }

	auto& primitives() const { return primitives_; }
	auto& materials() const { return materials_; }
	auto& images() const { return images_; }
	auto& samplers() const { return samplers_; }
	auto& defaultSampler() const { return defaultSampler_; }
	auto& dsLayout() const { return dsLayout_; }
	auto defaultMaterialID() const { return defaultMaterialID_; }

	u32 addPrimitive(std::vector<nytl::Vec3f> positions,
		std::vector<nytl::Vec3f> normals,
		std::vector<u32> indices,
		std::vector<nytl::Vec2f> texCoords0 = {},
		std::vector<nytl::Vec2f> texCoords1 = {});
	u32 addMaterial(const Material&);
	u32 addInstance(const Primitive&, nytl::Mat4f matrix, u32 matID);
	u32 addInstance(u32 primivieID, nytl::Mat4f matrix, u32 matID);

	void updatedInstance(u32 ini) { updateInis_.push_back(ini); }

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
	void writeInstance(const Instance& ini, nytl::Span<std::byte>& ids,
		nytl::Span<std::byte>& cmds);
	vk::Semaphore upload();

	bool multiDrawIndirect_ {};
	vpp::Sampler defaultSampler_;
	std::vector<Sampler> samplers_;
	std::vector<SceneImage> images_;
	std::vector<Material> materials_;
	std::vector<Primitive> primitives_;
	std::vector<Instance> instances_;
	unsigned defaultMaterialID_ {};
	unsigned instanceID_ {};
	unsigned indexCount_ {}; // total
	unsigned vertexCount_ {}; // total

	// for updateDevice
	unsigned newPrimitives_ {};
	unsigned newMats_ {};
	unsigned newInis_ {};
	std::vector<u32> updateInis_;

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

	// keep-alive during copying
	struct {
		vpp::SubBuffer stage;

		// when a material is added
		vpp::SubBuffer materials;

		// when a primitive is added
		vpp::SubBuffer indices;
		vpp::SubBuffer vertices;
	} upload_;
};

// Tries to parse the given string as path or filename of a gltf/gltb file
// and parse it. On success, also returns the base path as second parameter
std::tuple<std::optional<gltf::Model>, std::string> loadGltf(nytl::StringParam);

} // namespace tkn
