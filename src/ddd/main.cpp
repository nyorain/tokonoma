#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/bits.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/formats.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/bufferOps.hpp>

#include <dlg/dlg.hpp>
#include <nytl/mat.hpp>

#include <tinygltf.hpp>

#include <shaders/model.vert.h>
#include <shaders/model.frag.h>

class ViewApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		// Load Model
		namespace gltf = tinygltf;
		const auto filename = "../assets/gltf/test.gltf";
		dlg_info(">> Loading model...");

		gltf::TinyGLTF loader;
		gltf::Model model;
		std::string err;
		std::string warn;
		auto res = loader.LoadASCIIFromFile(&model, &err, &warn,
			filename);

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

			// TODO: assumptions that cannot be made in general
			// without them, we couldnt simply memcpy below
			dlg_assert(na.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);
			dlg_assert(na.type == TINYGLTF_TYPE_VEC3);
			dlg_assert(pa.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT);
			dlg_assert(pa.type == TINYGLTF_TYPE_VEC3);
			dlg_assert(ia.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
			dlg_assert(ia.type == TINYGLTF_TYPE_SCALAR);

			// write indices
			auto& ibv = model.bufferViews[ia.bufferView];
			auto& ib = model.buffers[ibv.buffer];
			// dlg_info(">> indices");
			for(auto i = 0u; i < ia.count; ++i) {
				auto address = ia.byteOffset + ibv.byteOffset + i * ia.ByteStride(ibv);
				auto v = *reinterpret_cast<const std::uint16_t*>(&ib.data[address]);
				// dlg_info("\t{} (from {})", v, address);
				doi::write(span, std::uint32_t(v));
			}
			// dlg_info(">> end indices");

			// write vertices and normals
			auto& pbv = model.bufferViews[pa.bufferView];
			auto& pb = model.buffers[pbv.buffer];
			auto& nbv = model.bufferViews[na.bufferView];
			auto& nb = model.buffers[nbv.buffer];
			dlg_assert(na.count == pa.count);

			// dlg_info(">> verts");
			for(auto i = 0u; i < pa.count; ++i) {
				auto paddress = pa.byteOffset + pbv.byteOffset + i * pa.ByteStride(pbv);
				auto naddress = na.byteOffset + nbv.byteOffset + i * na.ByteStride(nbv);
				auto p = *reinterpret_cast<const nytl::Vec3f*>(&pb.data[paddress]);
				auto n = *reinterpret_cast<const nytl::Vec3f*>(&nb.data[naddress]);
				doi::write(span, p);
				doi::write(span, n);
				// dlg_info("\tp: {}", p);
				// dlg_info("\tn: {}", n);
			}
			// dlg_info(">> end verts");

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

		// === Init pipeline ===
		auto gfxBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment,
				0),
		};

		dsLayout_ = {dev, gfxBindings};
		pipeLayout_ = {dev, {dsLayout_}, {}};

		vpp::ShaderModule vertShader(dev, model_vert_data);
		vpp::ShaderModule fragShader(dev, model_frag_data);
		vpp::GraphicsPipelineInfo gpi {renderer().renderPass(), pipeLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}, 0, renderer().samples()};

		struct Vertex {
			nytl::Vec3f pos;
			nytl::Vec3f normal;
		};
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

		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		pipe_ = {dev, vkpipe};

		// == ubo and stuff ==
		auto uboSize = 2 * sizeof(nytl::Mat4f) + sizeof(nytl::Vec3f) * 3;
		ds_ = {dev.descriptorAllocator(), dsLayout_};
		ubo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, 0, hostMem};

		vpp::DescriptorSetUpdate dsupdate(ds_);
		dsupdate.uniform({{ubo_.buffer(), ubo_.offset(), ubo_.size()}});
		vpp::apply({dsupdate});

		// write ubo
		{
			// auto mat = doi::frustum3Sym(0.1f, 0.1f, 0.1f, 100.f);
			// auto mat = doi::ortho3Sym(0.1f, 0.1f, 0.1f, 1000.f);
			using nytl::constants::pi;
			auto aspect = 1.f; // TODO
			auto mat = doi::perspective3<float>(pi / 2, aspect, 0.01f, 100.f);
			auto map = ubo_.memoryMap();
			auto span = map.span();
			doi::write(span, mat);
		}

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindIndexBuffer(cb, vertices_.buffer(), vertices_.offset(),
			vk::IndexType::uint32);
		auto vOffset = drawCount_ * sizeof(uint32_t);
		vk::cmdBindVertexBuffers(cb, 0, 1, {vertices_.buffer()},
			{vertices_.offset() + vOffset});
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			pipeLayout_, 0, {ds_}, {});
		vk::cmdDrawIndexed(cb, drawCount_, 1, 0, 0, 0);
	}

	void update(double delta) override {
		App::update(delta);
		App::redraw();
		time_ += delta;
	}

	void updateDevice() override {
		auto map = ubo_.memoryMap();
		auto span = map.span();
		doi::skip(span, sizeof(nytl::Mat4f));

		auto mat = doi::rotateMat<4, float>({1.f, -1.f, 0.2f}, time_);
		// translate into screen
		mat[2][3] = 5.f;
		doi::write(span, mat);
	}

	bool needsDepth() const override {
		return true;
	}

protected:
	vpp::SubBuffer vertices_;
	vpp::SubBuffer ubo_;

	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;

	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;

	unsigned drawCount_;
	float time_;
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({"3D View", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
