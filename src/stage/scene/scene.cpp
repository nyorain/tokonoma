#include <stage/scene/scene.hpp>
#include <stage/scene/primitive.hpp>
#include <stage/scene/material.hpp>
#include <stage/quaternion.hpp>
#include <stage/transform.hpp>
#include <stage/texture.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>

namespace doi {
namespace {

vpp::ViewableImage loadImage(const vpp::Device& dev, const gltf::Image& tex,
		nytl::StringParam path) {
	// TODO: simplifying assumptions atm
	// at least support other formast (r8, r8g8, r8g8b8, r32, r32g32b32,
	// maybe laso 16-bit formats)
	if(!tex.uri.empty()) {
		auto full = std::string(path);
		full += tex.uri;
		return doi::loadTexture(dev, full);
	}

	dlg_assert(tex.component == 4);
	dlg_assert(!tex.as_is);

	vk::Extent3D extent;
	extent.width = tex.width;
	extent.height = tex.height;
	extent.depth = 1;

	auto format = vk::Format::r8g8b8a8Srgb;
	auto dataSize = extent.width * extent.height * 4u;
	auto ptr = reinterpret_cast<const std::byte*>(tex.image.data());
	auto data = nytl::Span<const std::byte>(ptr, dataSize);

	return doi::loadTexture(dev, extent, format, data);
}

} // anon namespace

Scene::Scene(vpp::Device& dev, nytl::StringParam path,
		const tinygltf::Model& model, const tinygltf::Scene& scene,
		nytl::Mat4f matrix, const SceneRenderInfo& ri) {
	// TODO: lazy image loading? we currently load all, even when
	// maybe not needed
	// load images
	dlg_info("Found {} images", model.materials.size());
	for(auto& img : model.images) {
		dlg_info("  Loading image '{}'...", img.name);
		images_.push_back(loadImage(dev, img, path));
	}

	// load materials
	dlg_info("Found {} materials", model.materials.size());
	for(auto& material : model.materials) {
		dlg_info("  Loading material '{}'...", material.name);
		auto m = Material(dev, model, material,
			ri.materialDsLayout, ri.dummyTex, images_);
		materials_.push_back(std::move(m));
	}

	// we need at least one material (see primitive creation)
	// if there is none, add dummy
	if(materials_.empty()) {
		materials_.emplace_back(dev, ri.materialDsLayout, ri.dummyTex);
	}

	// load nodes tree recursively
	for(auto& nodeid : scene.nodes) {
		dlg_assert(unsigned(nodeid) < model.nodes.size());
		auto& node = model.nodes[nodeid];
		loadNode(dev, model, node, ri, matrix);
	}
}

void Scene::loadNode(vpp::Device& dev, const tinygltf::Model& model,
		const tinygltf::Node& node, const SceneRenderInfo& ri,
		nytl::Mat4f matrix) {

	if(!node.matrix.empty()) {
		nytl::Mat4f mat;
		for(auto r = 0u; r < 4; ++r) {
			for(auto c = 0u; c < 4; ++c) {
				// node.matrix is column major
				mat[r][c] = node.matrix[c * 4 + r];
			}
		}

		matrix = matrix * mat;
	} else if(!node.scale.empty() ||
			!node.translation.empty() ||
			!node.rotation.empty()) {
		// gltf: matrix computed as T * R * S
		auto mat = nytl::identity<4, float>();
		if(!node.scale.empty()) {
			mat[0][0] = node.scale[0];
			mat[1][1] = node.scale[1];
			mat[2][2] = node.scale[2];
		}

		if(!node.rotation.empty()) {
			doi::Quaternion q;
			q.x = node.rotation[0];
			q.y = node.rotation[1];
			q.z = node.rotation[2];
			q.w = node.rotation[3];

			mat = doi::toMat<4>(q) * mat;
		}

		if(!node.translation.empty()) {
			nytl::Vec3f t;
			t.x = node.translation[0];
			t.y = node.translation[1];
			t.z = node.translation[2];
			mat = doi::translateMat(t) * mat;
		}

		matrix = matrix * mat;
	}

	for(auto nodeid : node.children) {
		auto& child = model.nodes[nodeid];
		loadNode(dev, model, child, ri, matrix);
	}

	if(node.mesh != -1) {
		auto& mesh = model.meshes[node.mesh];
		dlg_info("  Loading mesh '{}'...", mesh.name);
		for(auto& primitive : mesh.primitives) {
			Material* mat;
			if(primitive.material < 0) {
				// there is always at least one material, see material
				// loading section. May be dummy material
				mat = &materials_[0];
			} else {
				auto mid = unsigned(primitive.material);
				dlg_assert(mid <= materials_.size());
				mat = &materials_[mid];
			}

			auto p = Primitive(dev, model, primitive,
				ri.primitiveDsLayout, *mat, matrix);
			primitives_.push_back(std::move(p));
		}
	}
}

void Scene::render(vk::CommandBuffer cb, vk::PipelineLayout pl) {
	for(auto& p : primitives_) {
		p.render(cb, pl, false); // TODO: don't care about shadow
	}
}

} // namespace doi
