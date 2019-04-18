#include <stage/scene/material.hpp>
#include <dlg/dlg.hpp>
#include <vpp/vk.hpp>
#include <stage/texture.hpp>

namespace doi {

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
		vk::ImageView dummy) {
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
		textured_ = true;
	} else {
		albedoTex_ = dummyView;
	}

	if(auto rm = pbr.find("metallicRoughnessTexture"); rm != pbr.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		dlg_assert(src < images.size());
		metalnessRoughnessTex_ = images[src].vkImageView();
		textured_ = true;
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
		textured_ = true;
		normalmap_ = true;
	} else {
		normalTex_ = dummyView;
	}

	if(auto rm = add.find("occlusionTexture"); rm != add.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		unsigned src = model.textures[tex].source;
		dlg_assert(src < images.size());
		occlusionTex_ = images[src].vkImageView();
		textured_ = true;
	} else {
		occlusionTex_ = dummyView;
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
	return textured_;
}

void Material::bind(vk::CommandBuffer cb, vk::PipelineLayout pl) const {
	struct {
		nytl::Vec4f albedo;
		float roughness;
		float metalness;
		std::uint32_t has_normal;
	} data = {
		albedo_, roughness_, metalness_, (normalmap_ ? 1u : 0u)
	};

	vk::cmdPushConstants(cb, pl, vk::ShaderStageBits::fragment, 0,
		sizeof(data), &data);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, 1, {ds_}, {});
}

} // namespace doi
