#include <stage/app.hpp>
#include <stage/bits.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/camera.hpp>
#include <stage/types.hpp>

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

#include <shaders/stage.simple3.vert.h>
#include <shaders/stage.color.frag.h>
#include <shaders/cloth.cloth.comp.h>
#include <shaders/cloth.apply.comp.h>

// TODO:
// - make all values in `params_` configurable via simple gui (vui::dat)
// - allow to choose grid size via command line args

using namespace doi::types;
using nytl::Vec3f;

// TODO: better third person camera
inline auto matrix3(const doi::Camera& c) {
	return projection(c) * doi::lookAtRH(-c.dir, {}, c.up);
}

class ClothApp : public doi::App {
public:
	struct Node {
		nytl::Vec3f pos;
		nytl::Vec3f vel;

		// integratin
		nytl::Vec3f lpos {};
		nytl::Vec3f npos {};
	};

public:
	bool init(nytl::Span<const char*> args) override {
		if(!doi::App::init(args)) {
			return false;
		}

		auto& dev = vulkanDevice();

		// init nodes
		nodes_.resize(gridSize_ * gridSize_);
		auto start = -int(gridSize_) / 2.f;
		for(auto y = 0u; y < gridSize_; ++y) {
			for(auto x = 0u; x < gridSize_; ++x) {
				auto pos =  nytl::Vec3f{start + x, 0, start + y};
				dlg_info(pos);
				nodes_[id(x, y)] = {pos, {0.f, 0.f, 0.f}, pos, pos};
			}
		}

		// pipeline
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex)
		};

		gfx_.dsLayout = {dev, bindings};
		gfx_.pipeLayout = {dev, {{gfx_.dsLayout.vkHandle()}}, {}};

		vpp::ShaderModule vertShader{dev, stage_simple3_vert_data};
		vpp::ShaderModule fragShader{dev, stage_color_frag_data};
		vpp::GraphicsPipelineInfo gpi {renderPass(), gfx_.pipeLayout, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}};

		vk::VertexInputAttributeDescription attribs[1];
		attribs[0].format = vk::Format::r32g32b32Sfloat;

		vk::VertexInputBindingDescription bufs[1];
		bufs[0].inputRate = vk::VertexInputRate::vertex;
		bufs[0].stride = sizeof(Vec3f);

		gpi.vertex.pVertexAttributeDescriptions = attribs;
		gpi.vertex.vertexAttributeDescriptionCount = 1;
		gpi.vertex.pVertexBindingDescriptions = bufs;
		gpi.vertex.vertexBindingDescriptionCount = 1;

		gpi.assembly.topology = vk::PrimitiveTopology::lineStrip;
		gpi.assembly.primitiveRestartEnable = true;
		gpi.rasterization.polygonMode = vk::PolygonMode::line;

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

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		auto indexStage = vpp::fillStaging(cb, indexBuf_,
			nytl::as_bytes(nytl::span(inds)));

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// init buffer
		auto bufSize = gridSize_ * gridSize_ * sizeof(Vec3f);
		nodesBuf_ = {dev.bufferAllocator(), bufSize,
			vk::BufferUsageBits::vertexBuffer, dev.hostMemoryTypes()};

		// ubo, ds
		auto uboSize = sizeof(nytl::Mat4f);
		gfx_.ubo = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};

		gfx_.ds = {dev.descriptorAllocator(), gfx_.dsLayout};
		vpp::DescriptorSetUpdate dsu(gfx_.ds);
		dsu.uniform({{{gfx_.ubo}}});

		return true;
	}

	std::size_t id(unsigned x, unsigned y) {
		dlg_assert(x < gridSize_ && y < gridSize_);
		return x * gridSize_ + y;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfx_.pipe);
		doi::cmdBindGraphicsDescriptors(cb, gfx_.pipeLayout, 0, {gfx_.ds});
		vk::cmdBindVertexBuffers(cb, 0, {{nodesBuf_.buffer().vkHandle()}},
			{{nodesBuf_.offset()}});
		vk::cmdBindIndexBuffer(cb, indexBuf_.buffer(), indexBuf_.offset(),
			vk::IndexType::uint32);
		vk::cmdDrawIndexed(cb, indexCount_, 1, 0, 0, 0);
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			doi::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
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

	// actually returns spring force plus dampening force
	Vec3f springForce(const Node& a, const Node& b, float ks, float kd, float l) {
		auto pd = b.pos - a.pos;
		auto vd = b.vel - a.vel;

		auto pl = nytl::length(pd);
		dlg_assert(pl > 0.0);
		pd *= 1.f / pl; // normalize
		return (ks * (pl - l) + kd * dot(vd, pd)) * pd;
	}

	void step(float dt) {
		#pragma omp parallel for
		for(auto x = 0u; x < gridSize_; ++x) {
			for(auto y = 0u; y < gridSize_; ++y) {
				if(x == 0 && y == 0 && params_.fixCorners[0]) {
					continue;
				} else if(x == gridSize_ - 1 && y == 0 && params_.fixCorners[1]) {
					continue;
				} else if(x == 0 && y == gridSize_ - 1 && params_.fixCorners[2]) {
					continue;
				} else if(x == gridSize_ - 1 && y == gridSize_ - 1 && params_.fixCorners[3]) {
					continue;
				}

				// start with external: gravity
				nytl::Vec3f f {0.f, -10.f * params_.mass, 0.f};
				auto& n = nodes_[id(x,y)];
				auto kd = params_.kd[0];
				auto ks = params_.ks[0];
				auto l = 1.f;
				auto addf = [&](int offx, int offy) {
					if(int(x) + offx >= int(gridSize_) ||
							int(y) + offy >= int(gridSize_) ||
							int(y) + offy < 0 ||
							int(x) + offx < 0) {
						return;
					}
					f += springForce(n, nodes_[id(x + offx, y + offy)],
						ks, kd, l);
				};

				addf(-1, 0);
				addf(1, 0);
				addf(0, -1);
				addf(0, 1);

				l = 2.f;
				kd = params_.kd[1];
				ks = params_.ks[1];

				addf(-2, 0);
				addf(2, 0);
				addf(0, -2);
				addf(0, 2);

				l = std::sqrt(2.f);
				kd = params_.kd[2];
				ks = params_.ks[2];

				addf(-1, -1);
				addf(1, 1);
				addf(-1, 1);
				addf(1, -1);

				// integrate
				f *= 1 / params_.mass;
				n.npos = 2 * n.pos - n.lpos + dt * dt * f;
			}
		}

		// update
		for(auto& n : nodes_) {
			n.lpos = n.pos;
			n.pos = n.npos;
			n.vel = (1.f / dt) * (n.pos - n.lpos);
		}
	}

	void update(double dt) override {
		App::update(dt);
		auto kc = appContext().keyboardContext();
		if(kc) {
			doi::checkMovement(camera_, *kc, dt);
		}

		// simulate
		// fixed time step
		accumdt_ += dt;
		auto total = 0u;
		while(accumdt_ > params_.stepdt) {
			if(++total > 3u) {
				dlg_warn("Updating too slow");
				accumdt_ = 0.f;
				break;
			}
			accumdt_ -= params_.stepdt;
			step(params_.stepdt);
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
			// doi::write(span, matrix3(camera_));
			doi::write(span, matrix(camera_));
			map.flush();
		}

		// always update position buffer
		auto map = nodesBuf_.memoryMap();
		auto span = map.span();

		auto size = 2.f;
		auto scale = size / gridSize_;
		for(auto y = 0u; y < gridSize_; ++y) {
			for(auto x = 0u; x < gridSize_; ++x) {
				auto& n = nodes_[id(x, y)];
				doi::write(span, nytl::Vec3f(scale * n.pos));
			}
		}

		map.flush();
	}

	const char* name() const override { return "cloth"; }
	bool needsDepth() const override { return true; }

protected:
	struct {
		vpp::SubBuffer ubo;
		vpp::TrDsLayout dsLayout;
		vpp::TrDs ds;
		vpp::PipelineLayout pipeLayout;
		vpp::Pipeline pipe;
	} gfx_;

	doi::Camera camera_;
	unsigned gridSize_ {40};
	std::vector<Node> nodes_;
	vpp::SubBuffer nodesBuf_;
	vpp::SubBuffer indexBuf_;
	bool rotateView_ {};
	unsigned indexCount_ {};

	float accumdt_ {};

	struct {
		std::array<float, 3> ks {50.f, 50.f, 50.f}; // spring dconstants
		std::array<float, 3> kd {0.03f, 0.03f, 0.03f}; // damping constants
		std::array<bool, 4> fixCorners {true, true, true, true};
		float stepdt {0.01}; // in s
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


