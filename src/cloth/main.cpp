#include <tkn/app.hpp>
#include <tkn/bits.hpp>
#include <tkn/render.hpp>
#include <tkn/window.hpp>
#include <tkn/camera.hpp>
#include <tkn/types.hpp>
#include <argagg.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/queue.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/vk.hpp>
#include <dlg/dlg.hpp>
#include <ny/mouseButton.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>
#include <vui/dat.hpp>
#include <vui/gui.hpp>

#include <shaders/tkn.simple3.vert.h>
#include <shaders/tkn.color.frag.h>
#include <shaders/cloth.cloth.comp.h>

// TODO: benchmark different work group sizes
// TODO: will probably more efficient to use textures for the nodes
//   instead of a buffer due to neighbor access.

using namespace tkn::types;
using nytl::Vec3f;

class ClothApp : public tkn::App {
public:
	struct Node {
		nytl::Vec4f pos;
		nytl::Vec4f vel;
	};

	static constexpr auto workGroupSize = 16u;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		auto& dev = vulkanDevice();

		// init pipe and stuff
		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});
		auto _t1 = initGfx(cb); // keep alive
		initComp();
		auto _t2 = uploadInitial(cb); // keep alive

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// init gui
		using namespace vui::dat;
		auto& gui = this->gui();
		auto pos = nytl::Vec2f {100.f, 0};
		auto& panel = gui.create<vui::dat::Panel>(pos, 300.f);

		auto createNumTextfield = [](auto& at, auto name, auto initial, auto func) {
			auto start = std::to_string(initial);
			if(start.size() > 6) {
				start.resize(6, '\0');
			}
			auto& t = at.template create<vui::dat::Textfield>(name, start).textfield();
			t.onSubmit = [&, f = std::move(func), name](auto& tf) {
				try {
					auto val = std::stof(std::string(tf.utf8()));
					f(val);
				} catch(const std::exception& err) {
					dlg_error("Invalid float for {}: {}", name, tf.utf8());
					return;
				}
			};

			return &t;
		};

		auto createValueTextfield = [createNumTextfield](auto& at, auto name,
				auto& value, bool* set = {}) {
			return createNumTextfield(at, name, value, [&value, set](auto v){
				value = v;
				if(set) {
					*set = true;
				}
			});
		};

		createValueTextfield(panel, "ks0", params_.ks[0], &updateParams_);
		createValueTextfield(panel, "ks1", params_.ks[1], &updateParams_);
		createValueTextfield(panel, "ks2", params_.ks[2], &updateParams_);

		createValueTextfield(panel, "kd0", params_.kd[0], &updateParams_);
		createValueTextfield(panel, "kd1", params_.kd[1], &updateParams_);
		createValueTextfield(panel, "kd2", params_.kd[2], &updateParams_);

		createValueTextfield(panel, "dt", params_.stepdt, &updateParams_);
		createValueTextfield(panel, "steps", iterations_, &rerecord_);
		createValueTextfield(panel, "mass", params_.mass, &updateParams_);

		for(auto i = 0u; i < 4; ++i) {
			auto name = "corner " + std::to_string(i);
			auto& cb = panel.create<Checkbox>(name).checkbox();
			cb.set(params_.fixCorners[i]);
			cb.onToggle = [=](auto&) {
				params_.fixCorners[i] ^= true;
				updateParams_ = true;
			};
		}

		auto& b = panel.create<Button>("Reset");
		b.onClick = [&]{
			resetCloth();
		};

		return true;
	}

	// returns temporary init stage
	[[nodiscard]] vpp::SubBuffer initGfx(vk::CommandBuffer cb) {
		auto& dev = vulkanDevice();

		// pipeline
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex)
		};

		gfx_.dsLayout = {dev, bindings};
		gfx_.pipeLayout = {dev, {{gfx_.dsLayout.vkHandle()}}, {}};

		vpp::ShaderModule vertShader{dev, tkn_simple3_vert_data};
		vpp::ShaderModule fragShader{dev, tkn_color_frag_data};
		vpp::GraphicsPipelineInfo gpi {renderPass(), gfx_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples()};

		vk::VertexInputAttributeDescription attribs[1];
		attribs[0].format = vk::Format::r32g32b32Sfloat;

		vk::VertexInputBindingDescription bufs[1];
		bufs[0].inputRate = vk::VertexInputRate::vertex;
		bufs[0].stride = sizeof(Node);

		gpi.vertex.pVertexAttributeDescriptions = attribs;
		gpi.vertex.vertexAttributeDescriptionCount = 1;
		gpi.vertex.pVertexBindingDescriptions = bufs;
		gpi.vertex.vertexBindingDescriptionCount = 1;

		gpi.assembly.topology = vk::PrimitiveTopology::lineStrip;
		gpi.assembly.primitiveRestartEnable = true;
		gpi.rasterization.polygonMode = vk::PolygonMode::line;
		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		gfx_.pipe = {dev, gpi.info()};

		// generate indices
		std::vector<u32> inds;
		for(auto y = 0u; y < gridSize_; ++y) {
			for(auto x = 0u; x < gridSize_; ++x) {
				inds.push_back(id(x, y));
			}

			inds.push_back(0xFFFFFFFFu);
		}

		for(auto x = 0u; x < gridSize_; ++x) {
			for(auto y = 0u; y < gridSize_; ++y) {
				inds.push_back(id(x, y));
			}

			inds.push_back(0xFFFFFFFFu);
		}

		indexCount_ = inds.size();
		indexBuf_ = {dev.bufferAllocator(), sizeof(u32) * inds.size(),
			vk::BufferUsageBits::indexBuffer | vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};

		auto indexStage = vpp::fillStaging(cb, indexBuf_,
			nytl::as_bytes(nytl::span(inds)));

		// ubo, ds
		auto uboSize = sizeof(nytl::Mat4f);
		gfx_.ubo = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		gfx_.ds = {dev.descriptorAllocator(), gfx_.dsLayout};
		vpp::DescriptorSetUpdate dsu(gfx_.ds);
		dsu.uniform({{{gfx_.ubo}}});

		return indexStage;
	}

	[[nodiscard]] vpp::SubBuffer uploadInitial(vk::CommandBuffer cb) {
		std::vector<Node> nodes;
		nodes.resize(gridSize_ * gridSize_);
		auto start = -int(gridSize_) / 2.f;
		for(auto y = 0u; y < gridSize_; ++y) {
			for(auto x = 0u; x < gridSize_; ++x) {
				auto pos =  nytl::Vec3f{start + x, 0, start + y};
				nodes[id(x, y)] = {nytl::Vec4f(pos), {0.f, 0.f, 0.f, 0.f}};
			}
		}

		auto data = nytl::as_bytes(nytl::span(nodes));
		auto stage = vpp::fillStaging(cb, nodesBuf_, data);
		return stage;
	}

	void initComp() {
		auto& dev = vulkanDevice();

		// pipeline
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
		};

		comp_.dsLayout = {dev, bindings};
		comp_.pipeLayout = {dev, {{comp_.dsLayout.vkHandle()}}, {}};

		vpp::ShaderModule compShader(device(), cloth_cloth_comp_data);
		vk::ComputePipelineCreateInfo cpi;
		cpi.layout = comp_.pipeLayout;
		cpi.stage.module = compShader;
		cpi.stage.pName = "main";
		cpi.stage.stage = vk::ShaderStageBits::compute;

		comp_.pipe = {dev, cpi};

		// init buffers
		auto bufSize = gridSize_ * gridSize_ * sizeof(Node);
		nodesBuf_ = {dev.bufferAllocator(), bufSize,
			vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferDst, dev.hostMemoryTypes()};
		tmpNodesBuf_ = {dev.bufferAllocator(), bufSize,
			vk::BufferUsageBits::storageBuffer, dev.hostMemoryTypes()};

		// ubo
		auto uboSize = 4 * 10u;
		comp_.ubo = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		comp_.ds[0] = {dev.descriptorAllocator(), comp_.dsLayout};
		comp_.ds[1] = {dev.descriptorAllocator(), comp_.dsLayout};

		vpp::DescriptorSetUpdate dsu0(comp_.ds[0]);
		dsu0.uniform({{{comp_.ubo}}});
		dsu0.storage({{{nodesBuf_}}});
		dsu0.storage({{{tmpNodesBuf_}}});

		vpp::DescriptorSetUpdate dsu1(comp_.ds[1]);
		dsu1.uniform({{{comp_.ubo}}});
		dsu1.storage({{{tmpNodesBuf_}}});
		dsu1.storage({{{nodesBuf_}}});
	}

	void resetCloth() {
		resetCloth_ = true;
	}

	std::size_t id(unsigned x, unsigned y) {
		dlg_assert(x < gridSize_ && y < gridSize_);
		return x * gridSize_ + y;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		// compute
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, comp_.pipe);
		auto c = std::ceil(gridSize_ / float(workGroupSize));

		std::array<vk::BufferMemoryBarrier, 2> barriers;
		barriers[0].buffer = nodesBuf_.buffer();
		barriers[0].offset = nodesBuf_.offset();
		barriers[0].size = nodesBuf_.size();
		barriers[0].srcAccessMask = vk::AccessBits::shaderRead;
		barriers[0].dstAccessMask = vk::AccessBits::shaderWrite;

		barriers[1].buffer = tmpNodesBuf_.buffer();
		barriers[1].offset = tmpNodesBuf_.offset();
		barriers[1].size = tmpNodesBuf_.size();
		barriers[1].srcAccessMask = vk::AccessBits::shaderWrite;
		barriers[1].dstAccessMask = vk::AccessBits::shaderRead;

		for(auto i = 0u; i < iterations_; ++i) {
			tkn::cmdBindComputeDescriptors(cb, comp_.pipeLayout, 0, {comp_.ds[0]});
			vk::cmdDispatch(cb, c, c, 1);

			vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
				vk::PipelineStageBits::computeShader, {}, {}, barriers, {});

			tkn::cmdBindComputeDescriptors(cb, comp_.pipeLayout, 0, {comp_.ds[1]});
			vk::cmdDispatch(cb, c, c, 1);

			std::swap(barriers[0].srcAccessMask, barriers[1].srcAccessMask);
			std::swap(barriers[0].dstAccessMask, barriers[1].dstAccessMask);
			vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
				vk::PipelineStageBits::computeShader |
				vk::PipelineStageBits::fragmentShader,
				{}, {}, barriers, {});
		}
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfx_.pipe);
		tkn::cmdBindGraphicsDescriptors(cb, gfx_.pipeLayout, 0, {gfx_.ds});
		vk::cmdBindVertexBuffers(cb, 0, {{nodesBuf_.buffer().vkHandle()}},
			{{nodesBuf_.offset()}});
		vk::cmdBindIndexBuffer(cb, indexBuf_.buffer(), indexBuf_.offset(),
			vk::IndexType::uint32);
		vk::cmdDrawIndexed(cb, indexCount_, 1, 0, 0, 0);

		rvgContext().bindDefaults(cb);
		gui().draw(cb);
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			tkn::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
			App::scheduleRedraw();
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

	void update(double dt) override {
		App::update(dt);
		auto kc = appContext().keyboardContext();
		if(kc) {
			tkn::checkMovement(camera_, *kc, dt);
		}

		// always redraw
		App::scheduleRedraw();
	}

	void updateDevice() override {
		App::updateDevice();

		if(camera_.update) {
			camera_.update = false;
			auto map = gfx_.ubo.memoryMap();
			auto span = map.span();

			auto scale = 2.f / gridSize_;
			auto mat = matrix(camera_) * tkn::scaleMat({scale, scale, scale});
			tkn::write(span, mat);
			map.flush();
		}

		if(updateParams_) {
			auto map = comp_.ubo.memoryMap();
			auto span = map.span();
			tkn::write(span, u32(gridSize_));
			tkn::write(span, params_.stepdt);
			tkn::write(span, params_.ks);
			tkn::write(span, params_.kd);
			tkn::write(span, params_.mass);

			u32 corners = 0u;
			for(auto i = 0u; i < 4u; ++i) {
				if(params_.fixCorners[i]) {
					corners |= (1u << i);
				}
			}

			tkn::write(span, corners);
			map.flush();
		}

		if(resetCloth_) {
			resetCloth_ = false;
			auto& dev = vulkanDevice();
			auto& qs = dev.queueSubmitter();
			auto qfam = qs.queue().family();
			auto cb = dev.commandAllocator().get(qfam);
			vk::beginCommandBuffer(cb, {});

			auto _t2 = uploadInitial(cb); // keep alive

			vk::endCommandBuffer(cb);
			qs.wait(qs.add(cb));
		}
	}

	argagg::parser argParser() const override {
		auto parser = App::argParser();
		parser.definitions.push_back({
			"size",
			{"-s", "--size"},
			"Size of the grid in each dimension", 1
		});
		return parser;
	}

	bool handleArgs(const argagg::parser_results& result) override {
		if (!App::handleArgs(result)) {
			return false;
		}

		if(result["size"].count()) {
			gridSize_ = result["size"].as<unsigned>();
		}

		return true;
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	const char* name() const override { return "cloth"; }
	bool needsDepth() const override { return true; }

protected:
	struct {
		vpp::SubBuffer ubo; // camera, mainly
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} gfx_;

	struct {
		vpp::SubBuffer ubo; // parameters
		vpp::Pipeline pipe;
		vpp::TrDsLayout dsLayout;
		std::array<vpp::TrDs, 2> ds; // for ping pong
		vpp::PipelineLayout pipeLayout;
	} comp_;

	tkn::Camera camera_;
	unsigned gridSize_ {64};
	vpp::SubBuffer nodesBuf_;
	vpp::SubBuffer tmpNodesBuf_;
	vpp::SubBuffer indexBuf_;
	bool rotateView_ {};
	unsigned indexCount_ {};

	float accumdt_ {};

	bool updateParams_ {true};
	bool resetCloth_ {false};
	unsigned iterations_ {16};
	struct {
		std::array<float, 3> ks {1.f, 1.f, 1.f}; // spring dconstants
		std::array<float, 3> kd {0.03f, 0.03f, 0.03f}; // damping constants
		std::array<bool, 4> fixCorners {true, true, true, true};
		float stepdt {0.001}; // in s
		float mass {0.05}; // in kg
	} params_;
};

int main(int argc, const char** argv) {
	ClothApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

