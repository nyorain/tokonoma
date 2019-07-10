#include <tkn/scene/material.hpp>
#include <tkn/scene/scene.hpp>
#include <dlg/dlg.hpp>
#include <vpp/vk.hpp>
#include <tkn/texture.hpp>

namespace tkn {

/*
vk::PushConstantRange Material::pcr() {
	vk::PushConstantRange pcr;
	pcr.offset = 0;
	pcr.size = sizeof(PCR);
	pcr.stageFlags = vk::ShaderStageBits::fragment;
	return pcr;
}

vpp::TrDsLayout Material::createDsLayout(const vpp::Device& dev) {
	auto bindings = {
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment), // albedo
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment), // metalRough
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment), // normal
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment), // occlusion
		vpp::descriptorBinding(
			vk::DescriptorType::combinedImageSampler,
			vk::ShaderStageBits::fragment), // emission
	};

	return {dev, bindings};
}

Material::Material(const vpp::TrDsLayout& dsLayout,
		vk::ImageView dummy, vk::Sampler dummySampler, nytl::Vec4f albedo,
		float roughness, float metalness, bool doubleSided,
		nytl::Vec3f emission) {
	pcr_.albedo = albedo;
	pcr_.roughness = roughness;
	pcr_.metalness = metalness;
	if(doubleSided) {
		pcr_.flags |= flagDoubleSided;
	}
	pcr_.emission = emission;

	albedoTex_ = {dummy, dummySampler};
	metalnessRoughnessTex_ = {dummy, dummySampler};
	normalTex_ = {dummy, dummySampler};
	occlusionTex_ = {dummy, dummySampler};
	emissionTex_ = {dummy, dummySampler};

	ds_ = {dsLayout.device().descriptorAllocator(), dsLayout};
	updateDs();
}

Material::Material(InitData& data, const gltf::Model& model,
		const gltf::Material& material, const vpp::TrDsLayout& dsLayout,
		vk::ImageView dummyView, Scene& scene) {
	ds_ = {data.initDs, dsLayout.device().descriptorAllocator(), dsLayout};

	auto& pbr = material.values;
	auto& add = material.additionalValues;
	if(auto color = pbr.find("baseColorFactor"); color != pbr.end()) {
		auto c = color->second.ColorFactor();
		pcr_.albedo = nytl::Vec4f{
			float(c[0]),
			float(c[1]),
			float(c[2]),
			float(c[3])};
	}

	if(auto roughness = pbr.find("roughnessFactor"); roughness != pbr.end()) {
		dlg_assert(roughness->second.has_number_value);
		pcr_.roughness = roughness->second.Factor();
	}

	if(auto metal = pbr.find("metallicFactor"); metal != pbr.end()) {
		dlg_assert(metal->second.has_number_value);
		pcr_.metalness = metal->second.Factor();
	}

	if(auto em = add.find("emissiveFactor"); em != add.end()) {
		auto c = em->second.ColorFactor();
		pcr_.emission = {float(c[0]), float(c[1]), float(c[2])};
	}

	auto getTex = [&](const gltf::Parameter& p, bool srgb,
			vk::Sampler& sampler, u32& coord) {
		coord = p.TextureTexCoord();
		dlg_assert(coord <= 1);
		if(coord == 0) {
			pcr_.flags |= flagNeedsTexCoords0;
		} else if(coord == 1) {
			pcr_.flags |= flagNeedsTexCoords1;
		}

		auto texid = p.TextureIndex();
		dlg_assert(texid != -1);
		auto& tex = model.textures[texid];

		sampler = scene.defaultSampler().vkHandle();
		if(tex.sampler >= 0) {
			dlg_assert(unsigned(tex.sampler) < scene.samplers().size());
			sampler = scene.samplers()[tex.sampler].sampler;
		}

		dlg_assert(tex.source >= 0);
		auto id = unsigned(tex.source);
		scene.createImage(id, srgb);
		return id;
	};

	if(auto tex = pbr.find("baseColorTexture"); tex != pbr.end()) {
		data.albedo = getTex(tex->second, true,
			albedoTex_.sampler, pcr_.albedoCoords);
	} else {
		albedoTex_ = {dummyView, scene.defaultSampler()};
	}

	if(auto tex = pbr.find("metallicRoughnessTexture"); tex != pbr.end()) {
		data.metalRoughness = getTex(tex->second, false,
			metalnessRoughnessTex_.sampler, pcr_.metalRoughCoords);
	} else {
		metalnessRoughnessTex_ = {dummyView, scene.defaultSampler()};
	}

	if(auto tex = add.find("normalTexture"); tex != add.end()) {
		data.normal = getTex(tex->second, false, normalTex_.sampler,
			pcr_.normalsCoords);
		pcr_.flags |= flagNormalMap;
	} else {
		normalTex_ = {dummyView, scene.defaultSampler()};
	}

	if(auto tex = add.find("occlusionTexture"); tex != add.end()) {
		data.occlusion = getTex(tex->second, false, occlusionTex_.sampler,
			pcr_.occlusionCoords);
	} else {
		occlusionTex_ = {dummyView, scene.defaultSampler()};
	}

	if(auto tex = add.find("emissiveTexture"); tex != add.end()) {
		data.emission = getTex(tex->second, true, emissionTex_.sampler,
			pcr_.emissionCoords);
	} else {
		emissionTex_ = {dummyView, scene.defaultSampler()};
	}

	// default is OPAQUE (alphaCutoff_ == -1.f);
	if(auto am = add.find("alphaMode"); am != add.end()) {
		auto& alphaMode = am->second.string_value;
		dlg_assert(!alphaMode.empty());
		if(alphaMode == "BLEND") {
			// NOTE: sorting and stuff is required for that to work...
			// TODO: we currently set alpha cutoff to a really small
			// value to at least somewhat mimic MASK mode in this case
			// as well. Probably not a good idea, just implement sorting
			dlg_warn("BLEND alphaMode not yet fully supported");
			pcr_.alphaCutoff = 0.0001f;
			pcr_.flags |= flagBlend;
		} else if(alphaMode == "MASK") {
			pcr_.alphaCutoff = 0.5; // default per gltf
			if(auto m = add.find("alphaCutoff"); m != add.end()) {
				dlg_assert(m->second.has_number_value);
				pcr_.alphaCutoff = m->second.number_value;
			}
		}
	}

	// doubleSided, default is false
	if(auto d = add.find("doubleSided"); d != add.end()) {
		if(d->second.bool_value) {
			pcr_.flags |= flagDoubleSided;
		}
	}
}

void Material::init(InitData& data, const Scene& scene) {
	if(!normalTex_.view) {
		dlg_assert(data.normal <= scene.images().size());
		normalTex_.view = scene.images()[data.normal].image.vkImageView();
	}
	if(!albedoTex_.view) {
		dlg_assert(data.albedo <= scene.images().size());
		albedoTex_.view = scene.images()[data.albedo].image.vkImageView();
	}
	if(!occlusionTex_.view) {
		dlg_assert(data.occlusion <= scene.images().size());
		occlusionTex_.view = scene.images()[data.occlusion].image.vkImageView();
	}
	if(!emissionTex_.view) {
		dlg_assert(data.emission <= scene.images().size());
		emissionTex_.view = scene.images()[data.emission].image.vkImageView();
	}
	if(!metalnessRoughnessTex_.view) {
		dlg_assert(data.metalRoughness <= scene.images().size());
		metalnessRoughnessTex_.view =
			scene.images()[data.metalRoughness].image.vkImageView();
	}

	ds_.init(data.initDs);
	updateDs();
}

void Material::updateDs() {
	vpp::DescriptorSetUpdate update(ds_);

	auto write = [&](auto& tex) {
		auto imgLayout = vk::ImageLayout::shaderReadOnlyOptimal;
		update.imageSampler({{tex.sampler, tex.view, imgLayout}});
	};

	write(albedoTex_);
	write(metalnessRoughnessTex_);
	write(normalTex_);
	write(occlusionTex_);
	write(emissionTex_);
}

void Material::bind(vk::CommandBuffer cb, vk::PipelineLayout pl) const {
	vk::cmdPushConstants(cb, pl, vk::ShaderStageBits::fragment, 0,
		sizeof(pcr_), &pcr_);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, 1, {{ds_.vkHandle()}}, {});
}
*/

} // namespace tkn
