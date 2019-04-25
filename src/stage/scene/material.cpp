#include <stage/scene/material.hpp>
#include <stage/scene/scene.hpp>
#include <dlg/dlg.hpp>
#include <vpp/vk.hpp>
#include <stage/texture.hpp>

namespace doi {
namespace {

vpp::ViewableImage loadImage(const vpp::Device& dev, const gltf::Image& tex,
		nytl::StringParam path, bool srgb) {
	// TODO: simplifying assumptions atm
	// at least support other formast (r8, r8g8, r8g8b8, r32, r32g32b32,
	// maybe also 16-bit formats)
	if(!tex.uri.empty()) {
		auto full = std::string(path);
		full += tex.uri;
		return doi::loadTexture(dev, full, false, srgb);
	}

	dlg_assert(tex.component == 4);
	dlg_assert(!tex.as_is);

	vk::Extent3D extent;
	extent.width = tex.width;
	extent.height = tex.height;
	extent.depth = 1;

	auto format = srgb ? vk::Format::r8g8b8a8Srgb : vk::Format::r8g8b8a8Unorm;
	auto dataSize = extent.width * extent.height * 4u;
	auto ptr = reinterpret_cast<const std::byte*>(tex.image.data());
	auto data = nytl::Span<const std::byte>(ptr, dataSize);

	return doi::loadTexture(dev, extent, format, data);
}

} // anon namespace


vk::PushConstantRange Material::pcr() {
	vk::PushConstantRange pcr;
	pcr.offset = 0;
	pcr.size = sizeof(float) * 8;
	pcr.stageFlags = vk::ShaderStageBits::fragment;
	return pcr;
}

vpp::TrDsLayout Material::createDsLayout(const vpp::Device& dev,
		vk::Sampler sampler) {
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, 0, 1, &sampler), // albedo
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, 1, 1, &sampler), // metalRough
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, 2, 1, &sampler), // normal
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment, 3, 1, &sampler), // occlusion
	};

	return {dev, bindings};
}

Material::Material(const vpp::Device& dev, const vpp::TrDsLayout& dsLayout,
		vk::ImageView dummy, nytl::Vec4f albedo, float roughness,
		float metalness, bool doubleSided) {
	albedo_ = albedo;
	roughness_ = roughness;
	metalness_ = metalness;
	if(doubleSided) {
		flags_ |= Flags::doubleSided;
	}

	// descriptor
	ds_ = {dev.descriptorAllocator(), dsLayout};
	vpp::DescriptorSetUpdate update(ds_);
	auto imgLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	albedoTex_ = dummy;
	metalnessRoughnessTex_ = dummy;
	normalTex_ = dummy;
	occlusionTex_ = dummy;

	update.imageSampler({{{}, albedoTex_, imgLayout}});
	update.imageSampler({{{}, metalnessRoughnessTex_, imgLayout}});
	update.imageSampler({{{}, normalTex_, imgLayout}});
	update.imageSampler({{{}, occlusionTex_, imgLayout}});
}

Material::Material(const vpp::Device& dev, const gltf::Model& model,
		const gltf::Material& material, const vpp::TrDsLayout& dsLayout,
		vk::ImageView dummyView, nytl::StringParam path,
		nytl::Span<SceneImage> images) {
	auto& pbr = material.values;
	auto& add = material.additionalValues;
	if(auto color = pbr.find("baseColorFactor"); color != pbr.end()) {
		auto c = color->second.ColorFactor();
		albedo_ = nytl::Vec4f{
			float(c[0]),
			float(c[1]),
			float(c[2]),
			float(c[3])};
	}

	if(auto roughness = pbr.find("roughnessFactor"); roughness != pbr.end()) {
		dlg_assert(roughness->second.has_number_value);
		roughness_ = roughness->second.Factor();
	}

	if(auto metal = pbr.find("metallicFactor"); metal != pbr.end()) {
		dlg_assert(metal->second.has_number_value);
		metalness_ = metal->second.Factor();
	}

	// TODO: repsect texture (sampler) parameters
	// TODO: we really should allocate textures at once, using
	// deferred initialization
	// TODO: loading images like that prevents us from parallalizing
	// image loading. Work on that. Maybe first make a list
	// of what images are needed, then parallalize loading (from disk),
	// then submit and wait for command buffers, then finish initialization
	// materials (i.e. write descriptor sets)
	auto getImage = [&](unsigned id, bool srgb) {
		dlg_assert(id < images.size());
		if(images[id].image.vkImageView()) {
			dlg_assert(images[id].srgb == srgb);
			return images[id].image.vkImageView();
		}

		images[id].image = loadImage(dev, model.images[id], path, srgb);
		images[id].srgb = srgb;
		return images[id].image.vkImageView();
	};

	if(auto color = pbr.find("baseColorTexture"); color != pbr.end()) {
		auto tex = color->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		albedoTex_ = getImage(src, true);
		flags_ |= Flags::textured;
	} else {
		albedoTex_ = dummyView;
	}

	if(auto rm = pbr.find("metallicRoughnessTexture"); rm != pbr.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		metalnessRoughnessTex_ = getImage(src, false);
		flags_ |= Flags::textured;
	} else {
		metalnessRoughnessTex_ = dummyView;
	}

	// TODO: we could also respect the factors for normal (?) and occlusion
	// we are completely ignoring emission atm
	if(auto rm = add.find("normalTexture"); rm != add.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		normalTex_ = getImage(src, false);
		flags_ |= Flags::textured;
		flags_ |= Flags::normalMap;
	} else {
		normalTex_ = dummyView;
	}

	if(auto rm = add.find("occlusionTexture"); rm != add.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		occlusionTex_ = getImage(src, false);
		flags_ |= Flags::textured;
	} else {
		occlusionTex_ = dummyView;
	}

	// default is OPAQUE (alphaCutoff_ == -1.f);
	if(auto am = add.find("alphaMode"); am != add.end()) {
		auto& alphaMode = am->second.string_value;
		dlg_assert(!alphaMode.empty());
		if(alphaMode == "BLEND") {
			// NOTE: sorting and stuff is required for that to work...
			dlg_warn("BLEND alphaMode not yet fully supported");
			alphaCutoff_ = 0.0001f; // TODO
		} else if(alphaMode == "MASK") {
			alphaCutoff_ = 0.5; // default per gltf
			if(auto m = add.find("alphaCutoff"); m != add.end()) {
				dlg_assert(m->second.has_number_value);
				alphaCutoff_ = m->second.number_value;
				dlg_info("{}", alphaCutoff_);
			}
		}
	}

	// doubleSided, default is false
	if(auto d = add.find("doubleSided"); d != add.end()) {
		if(d->second.bool_value) {
			flags_ |= Flags::doubleSided;
		}
	}

	// descriptor
	ds_ = {dev.descriptorAllocator(), dsLayout};
	vpp::DescriptorSetUpdate update(ds_);
	auto imgLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	update.imageSampler({{{}, albedoTex_, imgLayout}});
	update.imageSampler({{{}, metalnessRoughnessTex_, imgLayout}});
	update.imageSampler({{{}, normalTex_, imgLayout}});
	update.imageSampler({{{}, occlusionTex_, imgLayout}});
}

bool Material::hasTexture() const {
	return flags_ & Flags::textured;
}

void Material::bind(vk::CommandBuffer cb, vk::PipelineLayout pl) const {
	struct {
		nytl::Vec4f albedo;
		float roughness;
		float metalness;
		std::uint32_t flags;
		float alphaCutoff;
	} data = {
		albedo_,
		roughness_,
		metalness_,
		flags_.value(),
		alphaCutoff_
	};

	vk::cmdPushConstants(cb, pl, vk::ShaderStageBits::fragment, 0,
		sizeof(data), &data);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, 1, {ds_}, {});
}

} // namespace doi
