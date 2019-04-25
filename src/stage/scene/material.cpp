#include <stage/scene/material.hpp>
#include <dlg/dlg.hpp>
#include <vpp/vk.hpp>
#include <stage/texture.hpp>

namespace doi {

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
		vk::ImageView dummyView, nytl::Span<const vpp::ViewableImage> images) {
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

	if(auto color = pbr.find("baseColorTexture"); color != pbr.end()) {
		auto tex = color->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		dlg_assert(src < images.size());
		albedoTex_ = images[src].vkImageView();
		flags_ |= Flags::textured;
	} else {
		albedoTex_ = dummyView;
	}

	if(auto rm = pbr.find("metallicRoughnessTexture"); rm != pbr.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		dlg_assert(src < images.size());
		metalnessRoughnessTex_ = images[src].vkImageView();
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
		dlg_assert(src < images.size());
		normalTex_ = images[src].vkImageView();
		flags_ |= Flags::textured;
		flags_ |= Flags::normalMap;
	} else {
		normalTex_ = dummyView;
	}

	if(auto rm = add.find("occlusionTexture"); rm != add.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		dlg_assert(src < images.size());
		occlusionTex_ = images[src].vkImageView();
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
