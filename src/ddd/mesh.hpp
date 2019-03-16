#pragma once

#include <nytl/vec.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>

#include <tinygltf.hpp>

class Mesh {
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
	Mesh(const vpp::Device& dev,
		const tinygltf::Model& model, const tinygltf::Mesh& mesh,
		const vpp::TrDsLayout& dsLayout);

	void render(vk::CommandBuffer cb, vk::PipelineLayout pipeLayout);
	void updateDevice();

protected:
	unsigned indexCount {};
	unsigned vertexCount {};
	vpp::SubBuffer vertices; // indices + vertices
	vpp::SubBuffer ubo; // different buffer since on mappable mem
	vpp::TrDs ds;
};
