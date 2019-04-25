#pragma once

#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>

#include <tinygltf.hpp>

class Material;

class Primitive {
public:
	static constexpr auto uboSize = 2 * sizeof(nytl::Mat4f);
	struct Vertex {
		nytl::Vec3f pos;
		nytl::Vec3f normal;
	};

	// transform matrix
	// must call updateDevice when changed
	nytl::Mat4f matrix = nytl::identity<4, float>();

public:
	Primitive(const vpp::Device& dev,
		const tinygltf::Model& model, const tinygltf::Primitive& primitive,
		const vpp::TrDsLayout& dsLayout, const Material& material);

	void render(vk::CommandBuffer cb, vk::PipelineLayout pipeLayout,
		bool shadow);
	void updateDevice();
	const Material& material() const { return *material_; }

protected:
	unsigned indexCount_ {};
	unsigned vertexCount_ {};
	vpp::SubBuffer vertices_; // indices + vertices
	vpp::SubBuffer uv_; // uv coords (optional)
	vpp::SubBuffer ubo_; // different buffer since on mappable mem
	vpp::TrDs ds_;
	const Material* material_;
};
