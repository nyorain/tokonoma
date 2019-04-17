#include "material.hpp"
#include <dlg/dlg.hpp>
#include <vpp/vk.hpp>
#include <stage/texture.hpp>

namespace {

vpp::ViewableImage loadImage(const vpp::Device& dev, const gltf::Image& tex) {
	// TODO: simplifying assumptions atm
	if(!tex.uri.empty()) {
		return doi::loadTexture(dev, tex.uri);
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

Material::Material(const vpp::Device& dev, const gltf::Model& model,
		const gltf::Material& material, const vpp::TrDsLayout& dsLayout,
		vk::ImageView dummyView) {
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

	// TODO: we really should allocate textures at once, using
	// deferred initialization

	// descriptor
	ds_ = {dev.descriptorAllocator(), dsLayout};
	vpp::DescriptorSetUpdate update(ds_);
	auto imgLayout = vk::ImageLayout::shaderReadOnlyOptimal;

	if(auto color = pbr.find("baseColorTexture"); color != pbr.end()) {
		auto tex = color->second.TextureIndex();
		dlg_assert(tex != -1);
		auto& src = model.textures[tex].source;
		albedoTex_ = loadImage(dev, model.images[src]);
		update.imageSampler({{{}, albedoTex_.vkImageView(), imgLayout}});
	} else {
		dlg_trace("no albedo map");
		update.imageSampler({{{}, dummyView, imgLayout}});
	}

	if(auto rm = pbr.find("metallicRoughnessTexture"); rm != pbr.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		auto& src = model.textures[tex].source;
		metalnessRoughnessTex_ = loadImage(dev, model.images[src]);
		update.imageSampler({{{}, metalnessRoughnessTex_.vkImageView(),
			imgLayout}});
	} else {
		update.imageSampler({{{}, dummyView, imgLayout}});
	}

	// TODO: we could also respect the factors for normal (?) and occlusion
	// we are completely ignoring emission atm
	if(auto rm = add.find("normalTexture"); rm != add.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		auto& src = model.textures[tex].source;
		normalTex_ = loadImage(dev, model.images[src]);
		update.imageSampler({{{}, normalTex_.vkImageView(), imgLayout}});
	} else {
		dlg_trace("no normal map");
		update.imageSampler({{{}, dummyView, imgLayout}});
	}

	if(auto rm = add.find("occlusionTexture"); rm != add.end()) {
		auto tex = rm->second.TextureIndex();
		dlg_assert(tex != -1);
		auto& src = model.textures[tex].source;
		occlusionTex_ = loadImage(dev, model.images[src]);
		update.imageSampler({{{}, occlusionTex_.vkImageView(), imgLayout}});
	} else {
		dlg_trace("no occlusion map");
		update.imageSampler({{{}, dummyView, imgLayout}});
	}
}

bool Material::hasTexture() const {
	return occlusionTex_.vkImage() ||
		normalTex_.vkImage() ||
		metalnessRoughnessTex_.vkImage() ||
		albedoTex_.vkImage();
}

void Material::bind(vk::CommandBuffer cb, vk::PipelineLayout pl) const {
	struct {
		nytl::Vec4f albedo;
		float roughness;
		float metalness;
		std::uint32_t has_normal;
	} data = {
		albedo_, roughness_, metalness_, (normalTex_.vkImage() ? 1u : 0u)
	};

	vk::cmdPushConstants(cb, pl, vk::ShaderStageBits::fragment, 0,
		sizeof(data), &data);
	vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
		pl, 2, {ds_}, {});
}
