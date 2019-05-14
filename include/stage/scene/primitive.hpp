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

struct Shape; // stage/scene/shape.hpp
class Material; // stage/scene/material.hpp

class Primitive {
public:
	static constexpr auto uboSize = 2 * sizeof(nytl::Mat4f) +
		sizeof(std::uint32_t);
	struct Vertex {
		nytl::Vec3f pos;
		nytl::Vec3f normal;
	};

	struct InitData {
		vpp::SubBuffer stage;
		std::vector<std::byte> vertData;
		std::vector<std::byte> uvData;

		vpp::SubBuffer::InitData initStage;
		vpp::SubBuffer::InitData initVert;
		vpp::SubBuffer::InitData initUv;
		vpp::SubBuffer::InitData initUbo;
		vpp::TrDs::InitData initDs;
	};

	// transform matrix
	// must call updateDevice when changed
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
	void updateDevice();

	unsigned material() const { return material_; }
	bool hasUV() const { return uv_.size() != 0; }
	auto id() const { return id_; }

protected:
	unsigned indexCount_ {};
	unsigned vertexCount_ {};
	vpp::SubBuffer vertices_; // indices + vertices
	vpp::SubBuffer uv_; // uv coords (optional)
	vpp::SubBuffer ubo_; // different buffer since on mappable mem
	vpp::TrDs ds_;
	unsigned id_;
	unsigned material_; // id of the material
};

} // namespace doi
