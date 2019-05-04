#include <stage/scene/material.hpp>
#include <stage/scene/scene.hpp>
#include <dlg/dlg.hpp>
#include <vpp/vk.hpp>
#include <stage/texture.hpp>

namespace doi {


vk::PushConstantRange Material::pcr() {
	vk::PushConstantRange pcr;
	pcr.offset = 0;
	pcr.size = sizeof(float) * 11;
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
	albedo_ = albedo;
	roughness_ = roughness;
	metalness_ = metalness;
	if(doubleSided) {
		flags_ |= Flags::doubleSided;
	}
	emission_ = emission;

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
	ds_ = {vpp::defer, dsLayout.device().descriptorAllocator(), dsLayout};

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

	if(auto em = add.find("emissiveFactor"); em != add.end()) {
		auto c = em->second.ColorFactor();
		emission_ = {float(c[0]), float(c[1]), float(c[2])};
	}

	// TODO: we really should allocate textures at once, using
	// deferred initialization
	// TODO: loading images like that prevents us from parallalizing
	// image loading. Work on that. Maybe first make a list
	// of what images are needed, then parallalize loading (from disk),
	// then submit and wait for command buffers, then finish initialization
	// materials (i.e. write descriptor sets)
	auto getTex = [&](const gltf::Texture& tex, bool srgb, vk::Sampler& sampler) {
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

	if(auto color = pbr.find("baseColorTexture"); color != pbr.end()) {
		dlg_assertm(color->second.TextureTexCoord() == 0,
			"only one set of texture coordinates supported");
		auto tex = color->second.TextureIndex();
		dlg_assert(tex != -1);
		data.albedo = getTex(model.textures[tex], true, albedoTex_.sampler);
		flags_ |= Flags::textured;
	} else {
		albedoTex_ = {dummyView, scene.defaultSampler()};
	}

	if(auto rm = pbr.find("metallicRoughnessTexture"); rm != pbr.end()) {
		dlg_assertm(rm->second.TextureTexCoord() == 0,
			"only one set of texture coordinates supported");
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		data.metalRoughness = getTex(model.textures[tex], false,
			metalnessRoughnessTex_.sampler);
		flags_ |= Flags::textured;
	} else {
		metalnessRoughnessTex_ = {dummyView, scene.defaultSampler()};
	}

	// TODO: we could also respect the "strength" parameters of
	// (at least some?) textures
	if(auto rm = add.find("normalTexture"); rm != add.end()) {
		dlg_assertm(rm->second.TextureTexCoord() == 0,
			"only one set of texture coordinates supported");
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		data.normal = getTex(model.textures[tex], false, normalTex_.sampler);
		flags_ |= Flags::textured;
		flags_ |= Flags::normalMap;
	} else {
		normalTex_ = {dummyView, scene.defaultSampler()};
	}

	if(auto rm = add.find("occlusionTexture"); rm != add.end()) {
		dlg_assertm(rm->second.TextureTexCoord() == 0,
			"only one set of texture coordinates supported");
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		data.occlusion = getTex(model.textures[tex], false, occlusionTex_.sampler);
		flags_ |= Flags::textured;
	} else {
		occlusionTex_ = {dummyView, scene.defaultSampler()};
	}

	if(auto rm = add.find("emissiveTexture"); rm != add.end()) {
		dlg_assertm(rm->second.TextureTexCoord() == 0,
			"only one set of texture coordinates supported");
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		data.emission = getTex(model.textures[tex], true, emissionTex_.sampler);
		flags_ |= Flags::textured;
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
			alphaCutoff_ = 0.0001f;
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
}

void Material::initAlloc(const Scene& scene, InitData& data) {
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

	ds_.init();
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
		nytl::Vec3f emission;
	} data = {
		albedo_,
		roughness_,
		metalness_,
		flags_.value(),
		alphaCutoff_,
		emission_,
	};

	vk::cmdPushConstants(cb, pl, vk::ShaderStageBits::fragment, 0,
		sizeof(data), &data);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, 1, {ds_}, {});
}

} // namespace doi
