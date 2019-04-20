#include <stage/scene/scene.hpp>
#include <stage/scene/primitive.hpp>
#include <stage/scene/material.hpp>
#include <stage/scene/shape.hpp> // TODO(tmp)
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
	// maybe not needed. not a huge optimization though, most scenes
	// are probably optimized
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

	// TODO(tmp)
	materials_.emplace_back(dev, ri.materialDsLayout, ri.dummyTex,
		nytl::Vec4f {1.f, 1.f, 1.f, 1.f}, 1.f, 1.f, true);

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

	// TODO(tmp)
	// auto cube = doi::Cube{{}, {100.f, 100.f, 100.f}};
	// auto shape = doi::generate(cube);
	// primitives_.emplace_back(dev, shape, ri.primitiveDsLayout,
	// 	materials_.back(), nytl::identity<4, float>());
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

void Scene::render(vk::CommandBuffer cb, vk::PipelineLayout pl) const {
	for(auto& p : primitives_) {
		p.render(cb, pl);
	}
}

// util
bool has_suffix(const std::string_view& str, const std::string_view& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::unique_ptr<Scene> loadGltf(nytl::StringParam at, vpp::Device& dev,
		nytl::Mat4f matrix, const SceneRenderInfo& ri) {

	// Load Model
	// fallback
	std::string path = "../assets/gltf/";
	std::string file = "test3.gltf";
	bool binary = false;
	if(!at.empty()) {
		if(has_suffix(at, ".gltf")) {
			auto i = at.find_last_of('/');
			if(i == std::string::npos) {
				path = {};
				file = at;
			} else {
				path = at.substr(0, i + 1);
				file = at.substr(i + 1);
			}
		} else if(has_suffix(at, ".gltb")) {
			binary = true;
			auto i = at.find_last_of('/');
			if(i == std::string::npos) {
				path = {};
				file = at;
			} else {
				path = at.substr(0, i + 1);
				file = at.substr(i + 1);
			}
		} else {
			path = at;
			if(path.back() != '/') {
				path.push_back('/');
			}
			if(tinygltf::FileExists(path + "scene.gltf", nullptr)) {
				file = "scene.gltf";
			} else if(tinygltf::FileExists(path + "scene.gltb", nullptr)) {
				binary = true;
				file = "scene.gltb";
			} else {
				dlg_fatal("Given folder doesn't have scene.gltf/gltb");
				return {};
			}
		}
	}

	dlg_info(">> Parsing gltf model...");

	gltf::TinyGLTF loader;
	gltf::Model model;
	std::string err;
	std::string warn;

	auto full = std::string(path);
	full += file;
	bool res;
	if(binary) {
		res = loader.LoadBinaryFromFile(&model, &err, &warn, full.c_str());
	} else {
		res = loader.LoadASCIIFromFile(&model, &err, &warn, full.c_str());
	}

	// error, warnings
	auto pos = 0u;
	auto end = warn.npos;
	while((end = warn.find_first_of('\n', pos)) != warn.npos) {
		auto d = warn.data() + pos;
		dlg_warn("  {}", std::string_view{d, end - pos});
		pos = end + 1;
	}

	pos = 0u;
	while((end = err.find_first_of('\n', pos)) != err.npos) {
		auto d = err.data() + pos;
		dlg_error("  {}", std::string_view{d, end - pos});
		pos = end + 1;
	}

	if(!res) {
		dlg_fatal(">> Failed to parse model");
		return {};
	}

	dlg_info(">> Parsing Succesful...");

	// traverse nodes
	dlg_info("Found {} scenes", model.scenes.size());
	auto& sc = model.scenes[model.defaultScene];
	return std::make_unique<Scene>(dev, path, model, sc, matrix, ri);
}

} // namespace doi
