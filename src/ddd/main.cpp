#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/bits.hpp>

#include <ny/key.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>
#include <ny/mouseButton.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>

#include <dlg/dlg.hpp>
#include <nytl/mat.hpp>
#include <nytl/stringParam.hpp>

#include <tinygltf.hpp>

#include <shaders/model.vert.h>
#include <shaders/model.frag.h>
#include <shaders/shadowmap.vert.h>

// Matches glsl struct
struct Light {
	enum class Type : std::uint32_t {
		point = 1u,
		dir = 2u,
	};

	nytl::Vec3f pd; // position/direction
	Type type;
	nytl::Vec3f color;
	float _; // padding to match glsl
};

struct Camera {
	bool update = true;
	nytl::Vec3f pos {0.f, 0.f, 3.f};
	nytl::Vec3f dir {0.f, 0.f, -1.f};
	nytl::Vec3f up {0.f, 1.f, 0.f};

	struct {
		float fov = 0.48 * nytl::constants::pi;
		float aspect = 1.f;
		float near = 0.01f;
		float far = 10.f;
	} perspective;
};

auto matrix(Camera& c) {
	auto& p = c.perspective;

	// auto mat = doi::ortho3Sym<float>(10.f, 10.f, 0.1f, 10.f);
	auto mat = doi::perspective3RH<float>(p.fov, p.aspect, p.near, p.far);
	return mat * doi::lookAtRH(c.pos, c.pos + c.dir, c.up);
}

/// Throws std::runtime_error if componentType is not a valid gltf component type
/// Does not check for bounds of address
double read(const tinygltf::Buffer& buf, unsigned address, unsigned componentType) {
	double v;
	auto t = componentType;
	if(t == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
		v = *reinterpret_cast<const std::uint8_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
		v = *reinterpret_cast<const std::uint32_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
		v = *reinterpret_cast<const std::uint16_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_SHORT) {
		v = *reinterpret_cast<const std::int16_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_BYTE) {
		v = *reinterpret_cast<const std::int8_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_INT) {
		v = *reinterpret_cast<const std::int32_t*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_FLOAT) {
		v = *reinterpret_cast<const float*>(&buf.data[address]);
	} else if(t == TINYGLTF_COMPONENT_TYPE_DOUBLE) {
		v = *reinterpret_cast<const double*>(&buf.data[address]);
	} else {
		throw std::runtime_error("Invalid gltf component type");
	}

	return v;
}

// Reads as array
template<std::size_t N, typename T = float>
auto read(const tinygltf::Buffer& buf, unsigned address, unsigned type,
		unsigned componentType, T fill = T(0)) {
	std::array<T, N> vals;

	// NOTE: not really bytes though
	unsigned components = tinygltf::GetTypeSizeInBytes(type);
	unsigned valSize = tinygltf::GetComponentSizeInBytes(componentType);
	for(auto i = 0u; i < components; ++i) {
		if(i < components) {
			vals[i] = read(buf, address, componentType);
			address += valSize;
		} else {
			vals[i] = fill;
		}
	}

	return vals;
}

template<std::size_t N, typename T>
struct AccessorIterator {
	static_assert(N > 0);
	using Value = std::conditional_t<N == 1, T, nytl::Vec<N, T>>;

	const tinygltf::Buffer* buffer {};
	std::size_t address {};
	std::size_t stride {};
	unsigned type {};
	unsigned componentType {};

	AccessorIterator(const tinygltf::Model& model,
			const tinygltf::Accessor& accessor) {
		auto& bv = model.bufferViews[accessor.bufferView];
		buffer = &model.buffers[bv.buffer];
		address = accessor.byteOffset + bv.byteOffset;
		stride = accessor.ByteStride(bv);
		type = accessor.type;
		componentType = accessor.componentType;
	}

	AccessorIterator operator+(int value) const {
		auto cpy = *this;
		cpy.address += value * stride;
		return cpy;
	}

	AccessorIterator& operator+=(int value) {
		address += value * stride;
		return *this;
	}

	AccessorIterator operator-(int value) const {
		auto cpy = *this;
		cpy.address -= value * stride;
		return cpy;
	}

	AccessorIterator& operator-=(int value) {
		address -= value * stride;
		return *this;
	}

	AccessorIterator& operator++() {
		address += stride;
		return *this;
	}

	AccessorIterator operator++(int) {
		auto copy = *this;
		address += stride;
		return copy;
	}

	AccessorIterator& operator--() {
		address -= stride;
	}

	AccessorIterator operator--(int) {
		auto copy = *this;
		address -= stride;
		return copy;
	}

	Value operator*() const {
		return convert(read<N, T>(*buffer, address, type, componentType));
	}

	Value convert(const std::array<T, N>& val) const {
		Value ret;
		std::memcpy(&ret, val.data(), val.size() * sizeof(T));
		return ret;
	}
};

template<std::size_t N, typename T>
struct AccessorRange {
	using Iterator = AccessorIterator<N, T>;
	const tinygltf::Model& model;
	const tinygltf::Accessor& accessor;

	Iterator begin() const {
		return {model, accessor};
	}

	Iterator end() const {
		return begin() + accessor.count;
	}
};

template<std::size_t N, typename T>
AccessorRange<N, T> range(const tinygltf::Model& model,
		const tinygltf::Accessor& accessor) {
	return {model, accessor};
}

// TODO: just use <=> in C++20
template<std::size_t N, typename T>
bool operator==(const AccessorIterator<N, T>& a, const AccessorIterator<N, T>& b) {
	return a.buffer == b.buffer && a.address == b.address && a.stride == b.stride;
}

template<std::size_t N, typename T>
bool operator!=(const AccessorIterator<N, T>& a, const AccessorIterator<N, T>& b) {
	return a.buffer != b.buffer || a.address != b.address || a.stride != b.stride;
}

class ViewApp : public doi::App {
public:
	static constexpr auto maxLightSize = 8u;
	struct Vertex {
		nytl::Vec3f pos;
		nytl::Vec3f normal;
	};

public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		// Load Model
		const auto filename = "../assets/gltf/test2.gltf";
		if (!loadModel(filename)) {
			return false;
		}

		// == example light ==
		lights_.emplace_back();
		lights_.back().color = {1.f, 1.f, 1.f};
		lights_.back().pd = {1.8f, 2.3f, 2.f};
		lights_.back().type = Light::Type::point;

		// === Init pipeline ===
		auto& dev = vulkanDevice();
		auto hostMem = dev.hostMemoryTypes();

		// shadow sampler
		vk::SamplerCreateInfo sci {};
		sci.addressModeU = vk::SamplerAddressMode::clampToBorder;
		sci.addressModeV = vk::SamplerAddressMode::clampToBorder;
		sci.addressModeW = vk::SamplerAddressMode::clampToBorder;
		sci.borderColor = vk::BorderColor::floatOpaqueWhite;
		sci.magFilter = vk::Filter::linear;
		sci.minFilter = vk::Filter::linear;
		sci.minLod = 0.0;
		sci.maxLod = 0.25;
		sci.mipmapMode = vk::SamplerMipmapMode::nearest;
		sci.compareEnable = true;
		sci.compareOp = vk::CompareOp::lessOrEqual;
		shadow_.sampler = vpp::Sampler(dev, sci);

		// per scense; view + projection matrix, lights
		auto sceneBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
			vpp::descriptorBinding(
				vk::DescriptorType::combinedImageSampler,
				vk::ShaderStageBits::fragment),
		};

		sceneDsLayout_ = {dev, sceneBindings};

		// per object; model matrix and material stuff
		auto objectBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		objectDsLayout_ = {dev, objectBindings};

		pipeLayout_ = {dev, {sceneDsLayout_, objectDsLayout_}, {}};

		vk::SpecializationMapEntry maxLightsEntry;
		maxLightsEntry.size = sizeof(std::uint32_t);

		vk::SpecializationInfo fragSpec;
		fragSpec.dataSize = sizeof(std::uint32_t);
		fragSpec.pData = &maxLightSize;
		fragSpec.mapEntryCount = 1;
		fragSpec.pMapEntries = &maxLightsEntry;

		vpp::ShaderModule vertShader(dev, model_vert_data);
		vpp::ShaderModule fragShader(dev, model_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderer().renderPass(), pipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment, &fragSpec},
		}}, 0, renderer().samples()};

		constexpr auto stride = sizeof(Vertex);
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex
		};

		vk::VertexInputAttributeDescription attributes[2];
		attributes[0].format = vk::Format::r32g32b32Sfloat; // pos

		attributes[1].format = vk::Format::r32g32b32Sfloat; // normal
		attributes[1].offset = sizeof(float) * 3; // pos
		attributes[1].location = 1;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 2u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		pipe_ = {dev, vkpipe};

		// == ubo and stuff ==
		auto sceneUboSize = 2 * sizeof(nytl::Mat4f) // light; proj matrix
			+ maxLightSize * sizeof(Light) // lights
			+ sizeof(nytl::Vec3f) // viewPos
			+ sizeof(std::uint32_t); // numLights
		sceneDs_ = {dev.descriptorAllocator(), sceneDsLayout_};
		sceneUbo_ = {dev.bufferAllocator(), sceneUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		// object; XXX: perform per mesh (/node (?))
		auto objectUboSize = 2 * sizeof(nytl::Mat4f); // model, normal matrix
		objectDs_ = {dev.descriptorAllocator(), objectDsLayout_};
		objectUbo_ = {dev.bufferAllocator(), objectUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		initShadowPipe();

		// descriptors
		vpp::DescriptorSetUpdate sdsu(sceneDs_);
		sdsu.uniform({{sceneUbo_.buffer(), sceneUbo_.offset(), sceneUbo_.size()}});
		sdsu.imageSampler({{shadow_.sampler, shadow_.target.vkImageView(),
			vk::ImageLayout::depthStencilReadOnlyOptimal}});

		vpp::DescriptorSetUpdate odsu(objectDs_);
		odsu.uniform({{objectUbo_.buffer(), objectUbo_.offset(), objectUbo_.size()}});
		vpp::apply({sdsu, odsu});

		return true;
	}

	bool loadModel(nytl::StringParam filename) {
		dlg_info(">> Loading model...");
		namespace gltf = tinygltf;

		gltf::TinyGLTF loader;
		gltf::Model model;
		std::string err;
		std::string warn;
		auto res = loader.LoadASCIIFromFile(&model, &err, &warn, filename.c_str());

		// error, warnings
		auto pos = 0u;
		auto end = warn.npos;
		while((end = warn.find_first_of('\n', pos)) != warn.npos) {
			auto d = warn.data() + pos;
			dlg_warn("  {}", std::string_view{d, end - pos});
		}

		pos = 0u;
		while((end = err.find_first_of('\n', pos + 1)) != err.npos) {
			auto d = err.data() + pos;
			dlg_error("  {}", std::string_view{d, end - pos});
		}

		if(!res) {
			dlg_fatal(">> Failed to load model");
			return false;
		}

		dlg_info(">> Loading Succesful...");
		// load model onto gpu
		if(model.meshes.empty()) {
			dlg_fatal("No mesh to render");
			return false;
		}

		auto& mesh = model.meshes[0];
		if(mesh.primitives.empty()) {
			dlg_fatal("No primitive to render");
			return false;
		}

		auto& primitive = mesh.primitives[0];
		auto ip = primitive.attributes.find("POSITION");
		auto in = primitive.attributes.find("NORMAL");
		if(ip == primitive.attributes.end() || in == primitive.attributes.end()) {
			dlg_fatal("primitve doesn't have POSITION or NORMAL");
			return false;
		}

		auto& pa = model.accessors[ip->second];
		auto& na = model.accessors[in->second];
		auto& ia = model.accessors[primitive.indices];

		// compute total buffer size
		auto size = 0u;
		size += ia.count * sizeof(uint32_t); // indices
		size += na.count * sizeof(nytl::Vec3f); // normals
		size += pa.count * sizeof(nytl::Vec3f); // positions

		auto& dev = vulkanDevice();
		auto devMem = dev.deviceMemoryTypes();
		auto hostMem = dev.hostMemoryTypes();
		auto usage = vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::indexBuffer |
			vk::BufferUsageBits::transferDst;
		vertices_ = {dev.bufferAllocator(), size, usage, 0u, devMem};

		// fill it
		{
			auto stage = vpp::SubBuffer{dev.bufferAllocator(), size,
				vk::BufferUsageBits::transferSrc, 0u, hostMem};
			auto map = stage.memoryMap();
			auto span = map.span();

			// write indices
			for(auto idx : range<1, std::uint32_t>(model, ia)) {
				doi::write(span, idx);
			}

			// write vertices and normals
			dlg_assert(na.count == pa.count);
			auto pr = range<3, float>(model, pa);
			auto nr = range<3, float>(model, na);
			for(auto pit = pr.begin(), nit = nr.begin();
					pit != pr.end() && nit != nr.end();
					++nit, ++pit) {
				doi::write(span, *pit);
				doi::write(span, *nit);
			}

			// upload
			auto& qs = dev.queueSubmitter();
			auto cb = qs.device().commandAllocator().get(qs.queue().family());
			vk::beginCommandBuffer(cb, {});
			vk::BufferCopy region;
			region.dstOffset = vertices_.offset();
			region.srcOffset = stage.offset();
			region.size = size;
			vk::cmdCopyBuffer(cb, stage.buffer(), vertices_.buffer(), {region});
			vk::endCommandBuffer(cb);

			// execute
			// TODO: could be batched with other work; we wait here
			vk::SubmitInfo submission;
			submission.commandBufferCount = 1;
			submission.pCommandBuffers = &cb.vkHandle();
			qs.wait(qs.add(submission));
		}

		drawCount_ = ia.count;
		return true;
	}

	void initShadowPipe() {
		auto& dev = vulkanDevice();
		auto hostMem = dev.hostMemoryTypes();

		// renderpass
		vk::AttachmentDescription depth {};
		depth.initialLayout = vk::ImageLayout::undefined;
		depth.finalLayout = vk::ImageLayout::depthStencilReadOnlyOptimal;
		depth.format = shadow_.format;
		depth.loadOp = vk::AttachmentLoadOp::clear;
		depth.storeOp = vk::AttachmentStoreOp::store;
		depth.samples = vk::SampleCountBits::e1;

		vk::AttachmentReference depthRef {};
		depthRef.attachment = 0;
		depthRef.layout = vk::ImageLayout::depthStencilAttachmentOptimal;

		vk::SubpassDescription subpass {};
		subpass.pDepthStencilAttachment = &depthRef;

		vk::RenderPassCreateInfo rpi {};
		rpi.attachmentCount = 1;
		rpi.pAttachments = &depth;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &subpass;

		shadow_.rp = {dev, rpi};

		// target
		auto targetUsage = vk::ImageUsageBits::depthStencilAttachment |
			vk::ImageUsageBits::sampled;
		auto targetInfo = vpp::ViewableImageCreateInfo::general(dev,
			shadow_.extent, targetUsage, {shadow_.format},
			vk::ImageAspectBits::depth);
		shadow_.target = {dev, *targetInfo};

		// framebuffer
		vk::FramebufferCreateInfo fbi {};
		fbi.attachmentCount = 1;
		fbi.width = shadow_.extent.width;
		fbi.height = shadow_.extent.height;
		fbi.layers = 1u;
		fbi.pAttachments = &shadow_.target.vkImageView();
		fbi.renderPass = shadow_.rp;
		shadow_.fb = {dev, fbi};

		// layouts
		// XXX: using other layouts for now
		/*
		auto bindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex), // view/projection
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex) // model
		};

		shadow_.dsLayout = {dev, bindings};
		shadow_.pipeLayout = {dev, {shadow_.dsLayout}, {}};
		*/

		vpp::ShaderModule vertShader(dev, shadowmap_vert_data);
		vpp::GraphicsPipelineInfo gpi {shadow_.rp, pipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
		}}, 0, vk::SampleCountBits::e1};

		constexpr auto stride = sizeof(Vertex);
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex
		};

		vk::VertexInputAttributeDescription attributes[1];
		attributes[0].format = vk::Format::r32g32b32Sfloat; // pos

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 1u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;

		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;
		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.rasterization.depthBiasEnable = true;

		auto dynamicStates = {
			vk::DynamicState::depthBias,
			vk::DynamicState::viewport,
			vk::DynamicState::scissor
		};
		gpi.dynamic.pDynamicStates = dynamicStates.begin();
		gpi.dynamic.dynamicStateCount = 3;

		gpi.blend.attachmentCount = 0;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		shadow_.pipeline = {dev, vkpipe};

		// setup light ds and ubo
		auto lightUboSize = sizeof(nytl::Mat4f); // projection, view
		shadow_.ds = {dev.descriptorAllocator(), sceneDsLayout_};
		shadow_.ubo = {dev.bufferAllocator(), lightUboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		auto& lubo = shadow_.ubo;
		vpp::DescriptorSetUpdate ldsu(shadow_.ds);
		ldsu.uniform({{lubo.buffer(), lubo.offset(), lubo.size()}});
		vpp::apply({ldsu});

		// fill ubo once
		{
			auto map = shadow_.ubo.memoryMap();
			auto span = map.span();
			doi::write(span, lightMatrix());
		}
	}

	nytl::Mat4f lightMatrix() {
		auto& light = lights_[0];
		auto mat = doi::ortho3Sym(5.f, 5.f, 0.5f, 10.f);
		// auto mat = doi::perspective3RH<float>(0.25 * nytl::constants::pi, 1.f, 0.1, 5.f);
		mat = mat * doi::lookAtRH(light.pd,
			{0.f, 0.f, 0.f}, // always looks at center
			{0.f, 1.f, 0.f});
		return mat;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		App::beforeRender(cb);

		auto width = shadow_.extent.width;
		auto height = shadow_.extent.height;
		vk::ClearValue clearValue {};
		clearValue.depthStencil = {1.f, 0u};

		vk::RenderPassBeginInfo rpb; // TODO
		vk::cmdBeginRenderPass(cb, {
			shadow_.rp,
			shadow_.fb,
			{0u, 0u, width, height},
			1,
			&clearValue
		}, {});

		vk::Viewport vp {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
		vk::cmdSetViewport(cb, 0, 1, vp);
		vk::cmdSetScissor(cb, 0, 1, {0, 0, width, height});
		vk::cmdSetDepthBias(cb, 1.25, 0.f, 1.75);

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, shadow_.pipeline);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {shadow_.ds, objectDs_}, {});
		auto vOffset = drawCount_ * sizeof(uint32_t);
		vk::cmdBindVertexBuffers(cb, 0, 1, {vertices_.buffer()},
			{vertices_.offset() + vOffset});
		vk::cmdBindIndexBuffer(cb, vertices_.buffer(), vertices_.offset(),
			vk::IndexType::uint32);
		vk::cmdDrawIndexed(cb, drawCount_, 1, 0, 0, 0);

		vk::cmdEndRenderPass(cb);
	}

	void render(vk::CommandBuffer cb) override {
		// draw
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindIndexBuffer(cb, vertices_.buffer(), vertices_.offset(),
			vk::IndexType::uint32);
		auto vOffset = drawCount_ * sizeof(uint32_t);
		vk::cmdBindVertexBuffers(cb, 0, 1, {vertices_.buffer()},
			{vertices_.offset() + vOffset});
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {sceneDs_, objectDs_}, {});
		vk::cmdDrawIndexed(cb, drawCount_, 1, 0, 0, 0);
	}

	void update(double dt) override {
		App::update(dt);
		App::redraw();
		time_ += dt;

		// movement
		auto kc = appContext().keyboardContext();
		auto fac = dt;

		auto yUp = nytl::Vec3f {0.f, 1.f, 0.f};
		auto right = nytl::normalized(nytl::cross(camera_.dir, yUp));
		auto up = nytl::normalized(nytl::cross(camera_.dir, right));
		if(kc->pressed(ny::Keycode::d)) { // right
			camera_.pos += fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::a)) { // left
			camera_.pos += -fac * right;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::w)) {
			camera_.pos += fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::s)) {
			camera_.pos += -fac * camera_.dir;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::q)) { // up
			camera_.pos += -fac * up;
			camera_.update = true;
		}
		if(kc->pressed(ny::Keycode::e)) { // down
			camera_.pos += fac * up;
			camera_.update = true;
		}

		if(moveLight_) {
			lights_[0].pd.x = 2.0 * std::cos(time_);
			lights_[0].pd.z = 2.0 * std::sin(time_);
			updateLights_ = true;
		}
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		switch(ev.keycode) {
			case ny::Keycode::l:
				moveLight_ ^= true;
				return true;
			default:
				break;
		}

		return false;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			using nytl::constants::pi;
			yaw_ += 0.005 * ev.delta.x;
			pitch_ += 0.005 * ev.delta.y;
			pitch_ = std::clamp<float>(pitch_, -pi / 2 + 0.1, pi / 2 - 0.1);

			camera_.dir.x = std::sin(yaw_) * std::cos(pitch_);
			camera_.dir.y = -std::sin(pitch_);
			camera_.dir.z = -std::cos(yaw_) * std::cos(pitch_);
			nytl::normalize(camera_.dir);
			camera_.update = true;
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			rotateView_ = ev.pressed;
			return true;
		}

		return false;
	}

	void updateDevice() override {
		// update scene ubo
		if(camera_.update || updateLights_) {
			camera_.update = false;

			auto map = sceneUbo_.memoryMap();
			auto span = map.span();

			doi::write(span, matrix(camera_));
			doi::write(span, lightMatrix());

			{ // lights
				auto lspan = span;
				for(auto& l : lights_) {
					doi::write(lspan, l);
				}
			}

			doi::skip(span, sizeof(Light) * maxLightSize);
			doi::write(span, camera_.pos);
			doi::write(span, std::uint32_t(lights_.size()));
		}

		// update model matrix
		auto map = objectUbo_.memoryMap();
		auto span = map.span();

		// model matrix
		auto mat = nytl::identity<4, float>();
		doi::write(span, mat);
		auto normalMatrix = nytl::Mat4f(transpose(inverse(mat)));
		doi::write(span, normalMatrix);

		// write shadow ubo
		if(updateLights_) {
			updateLights_ = false;
			auto map = shadow_.ubo.memoryMap();
			auto span = map.span();
			doi::write(span, lightMatrix());
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	bool needsDepth() const override {
		return true;
	}

protected:
	vpp::SubBuffer vertices_;
	vpp::SubBuffer sceneUbo_;
	vpp::SubBuffer objectUbo_; // per object

	vpp::TrDsLayout sceneDsLayout_;
	vpp::TrDsLayout objectDsLayout_;
	vpp::TrDs sceneDs_;
	vpp::TrDs objectDs_; // per object

	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;

	bool moveLight_ {false};

	unsigned drawCount_;
	float time_ {};
	bool rotateView_ {false};

	std::vector<Light> lights_;
	bool updateLights_ {true};

	Camera camera_ {};
	float yaw_ {0.f};
	float pitch_ {0.f}; // rotation around x (left/right) axis

	// shadow
	struct {
		// static
		vpp::RenderPass rp;
		vpp::Sampler sampler; // with compareOp (?) glsl: sampler2DShadow
		// vpp::TrDsLayout sceneDsLayout;
		// vpp::TrDsLayout objectDsLayout;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipeline;
		vk::Format format = vk::Format::d32Sfloat; // TODO

		vk::Extent3D extent {1024u, 1024u, 1u};

		// per light
		vpp::ViewableImage target;
		vpp::Framebuffer fb;
		vpp::TrDs ds;
		vpp::SubBuffer ubo; // holding the light view matrix
	} shadow_;
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({"3D View", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
