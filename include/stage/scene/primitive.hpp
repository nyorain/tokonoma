#pragma once

#include <stage/defer.hpp>

#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>

#include <tinygltf.hpp>
#include <variant>

namespace doi {
namespace gltf = tinygltf;


/*
// TODO: allow instancing

struct Shape; // stage/scene/shape.hpp
class Material; // stage/scene/material.hpp

class Primitive {
public:
	struct Vertex {
		nytl::Vec3f pos;
		nytl::Vec3f normal;
	};

	struct InitData {
		vpp::SubBuffer stage;
		std::vector<std::byte> vertData;
		std::vector<std::byte> texCoord0Data;
		std::vector<std::byte> texCoord1Data;

		vpp::SubBuffer::InitData initStage;
		vpp::SubBuffer::InitData initVert;
		vpp::SubBuffer::InitData initTexCoords0;
		vpp::SubBuffer::InitData initTexCoords1;
	};

	static const vk::PipelineVertexInputStateCreateInfo& vertexInfo();

	// transform matrix
	// must call Scene::updateDevice when changed
	nytl::Mat4f matrix = nytl::identity<4, float>();

public:
	Primitive() = default;
	Primitive(const WorkBatcher& wb,
		const Shape&, const vpp::TrDsLayout&, unsigned material,
		const nytl::Mat4f& transform, unsigned id);
	Primitive(InitData&, const WorkBatcher& wb, const gltf::Model&,
		const gltf::Primitive&, const vpp::TrDsLayout&, unsigned material,
		const nytl::Mat4f& transform, unsigned id);

	void init(InitData&, vk::CommandBuffer);
	void render(vk::CommandBuffer cb, vk::PipelineLayout pipeLayout) const;

	unsigned material() const { return material_; }
	bool hasTexCoords0() const { return texCoords0_.size() != 0; }
	bool hasTexCoords1() const { return texCoords1_.size() != 0; }
	auto id() const { return id_; }

	auto indexCount() const { return indexCount_; }
	auto vertexCount() const { return vertexCount_; }
	nytl::Vec3f min() const { return min_; }
	nytl::Vec3f max() const { return max_; }

protected:
	unsigned indexCount_ {};
	unsigned vertexCount_ {};
	vpp::SubBuffer vertices_; // indices + vertices
	vpp::SubBuffer texCoords0_; // optional
	vpp::SubBuffer texCoords1_; // optional
	unsigned id_;
	unsigned material_; // id of the material
	nytl::Vec3f min_ {};
	nytl::Vec3f max_ {};
};
*/

} // namespace doi
