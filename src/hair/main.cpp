#include <tkn/app.hpp>
#include <tkn/bits.hpp>
#include <tkn/render.hpp>
#include <tkn/window.hpp>
#include <tkn/camera.hpp>
#include <tkn/types.hpp>
#include <tkn/geometry.hpp>
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
#include <ny/key.hpp>
#include <ny/appContext.hpp>
#include <nytl/math.hpp>

#include <shaders/tkn.simple2.vert.h>
#include <shaders/tkn.color.frag.h>

class HairApp : public tkn::App {
public:
	struct Node {
		nytl::Vec2f pos;
		nytl::Vec2f vel;
	};

	static constexpr auto nodeCount = 32u;
	static constexpr auto ks1 = 1.f;
	static constexpr auto ks2 = 1.f;
	static constexpr auto ks3 = 1.f;
	static constexpr auto kd1 = 0.05f;
	static constexpr auto kd2 = 0.05f;
	static constexpr auto kd3 = 0.05f;
	static constexpr auto kdg = 0.2f; // general dampening per second
	static constexpr auto mass = 0.1f;
	static constexpr auto segLength = 1.f / nodeCount;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		// init logic
		auto pos = nytl::Vec2f{0.f, -0.5f};
		for(auto i = 0u; i < nodeCount; ++i) {
			auto& node = nodes_.emplace_back();
			node.pos = pos;
			node.vel = {0.f, 0.f};
			pos.y += segLength;
		}

		// init gfx
		auto& dev = vulkanDevice();
		pipeLayout_ = {dev, {}, {}};
		vpp::ShaderModule vertShader{dev, tkn_simple2_vert_data};
		vpp::ShaderModule fragShader{dev, tkn_color_frag_data};

		vpp::GraphicsPipelineInfo gpi {renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples()};

		vk::VertexInputAttributeDescription attribs[1];
		attribs[0].format = vk::Format::r32g32Sfloat;

		vk::VertexInputBindingDescription bufs[1];
		bufs[0].inputRate = vk::VertexInputRate::vertex;
		bufs[0].stride = sizeof(nytl::Vec2f);

		gpi.vertex.pVertexAttributeDescriptions = attribs;
		gpi.vertex.vertexAttributeDescriptionCount = 1;
		gpi.vertex.pVertexBindingDescriptions = bufs;
		gpi.vertex.vertexBindingDescriptionCount = 1;

		gpi.assembly.topology = vk::PrimitiveTopology::lineStrip;
		gpi.rasterization.polygonMode = vk::PolygonMode::line;
		gpi.depthStencil.depthTestEnable = false;
		gpi.depthStencil.depthWriteEnable = false;

		pipe_ = {dev, gpi.info()};

		// vertex buffer
		// filled in every frame, see updateDevice
		vertices_ = {dev.bufferAllocator(), sizeof(nytl::Vec2f) * nodes_.size(),
			vk::BufferUsageBits::vertexBuffer, dev.hostMemoryTypes()};

		return true;
	}

	nytl::Vec2f normalized(nytl::Vec2i winPos) {
		return nytl::Vec2f{-1.f, -1.f} + 2.f *
			nytl::vec::cw::divide(winPos, nytl::Vec2f(window().size()));
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		mpos_ = normalized(ev.position);
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		vk::cmdBindVertexBuffers(cb, 0, {{vertices_.buffer().vkHandle()}},
			{{vertices_.offset()}});
		vk::cmdDraw(cb, nodes_.size(), 1, 0, 0);
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			mouseDown_ = ev.pressed;
			return true;
		}

		return false;
	}

	float distance(nytl::Vec2f point, tkn::Segment2f seg) {
		nytl::Vec2f ab = seg.b - seg.b;
		nytl::Vec2f ap = point - seg.a;
		float fac = dot(ap, ab) / dot(ab, ab);
		nytl::Vec2f clamped = seg.a + std::clamp(fac, 0.f, 1.f) * ab;
		return length(point - clamped);
	}

	void update(double dt) override {
		App::update(dt);

		auto force = [](auto& node1, auto& node2, float ks, float kd, float l) {
			auto diff = node2.pos - node1.pos;
			auto vdiff = node2.vel - node1.vel;
			auto ld = length(diff);
			diff *= 1 / ld;
			return (ks * (ld - l) + kd * dot(vdiff, diff)) * diff;
		};

		auto nextNodes = nodes_;
		for(auto i = 0u; i < nodes_.size(); ++i) {
			auto& node = nodes_[i];

			nytl::Vec2f f{0.f, 0.f};
			if(i > 0) {
				f += force(node, nodes_[i - 1], ks1, kd1, segLength);
				if(i > 1) {
					f += force(node, nodes_[i - 2], ks2, kd2, 2 * segLength);
					if(i > 2) {
						f += force(node, nodes_[i - 3], ks3, kd3, 3 * segLength);
					}

					// if(i > 3) f += force(node, nodes_[i - 4], ks3, kd3, 4 * segLength);
					// if(i > 4) f += force(node, nodes_[i - 5], ks3, kd3, 5 * segLength);
					// if(i > 5) f += force(node, nodes_[i - 6], ks3, kd3, 6 * segLength);
					// if(i > 6) f += force(node, nodes_[i - 7], ks3, kd3, 7 * segLength);
					// if(i > 7) f += force(node, nodes_[i - 8], ks3, kd3, 8 * segLength);
				}
			}

			if(i < nodes_.size() - 1) {
				f += force(node, nodes_[i + 1], ks1, kd1, segLength);
				if(i < nodes_.size() - 2) {
					f += force(node, nodes_[i + 2], ks2, kd2, 2 * segLength);
					if(i < nodes_.size() - 3) {
						f += force(node, nodes_[i + 3], ks3, kd3, 3 * segLength);
					}

					// if(i < nodes_.size() - 4) f += force(node, nodes_[i + 4], ks3, kd3, 4 * segLength);
					// if(i < nodes_.size() - 5) f += force(node, nodes_[i + 5], ks3, kd3, 5 * segLength);
					// if(i < nodes_.size() - 6) f += force(node, nodes_[i + 6], ks3, kd3, 6 * segLength);
					// if(i < nodes_.size() - 7) f += force(node, nodes_[i + 7], ks3, kd3, 7 * segLength);
					// if(i < nodes_.size() - 8) f += force(node, nodes_[i + 8], ks3, kd3, 8 * segLength);
				}
			}

			// f -= (1 - std::pow(kdg, dt)) * node.vel;
			f -= kdg * node.vel;
			auto a = (1 / mass) * f;

			// verlet-like integration
			auto& next = nextNodes[i];
			next.pos += dt * next.vel + 0.5 * dt * dt * a;
			next.vel += 0.5 * dt * a;

			// check for intersection
			/*
			for(auto j = 1u; j < i; ++j) {
				tkn::Segment2f line = {nextNodes[i - 1].pos, nextNodes[i].pos};
				tkn::Segment2f a = {nextNodes[j - 1].pos, nextNodes[j].pos};
				auto is = tkn::intersection(a, line);
				if(is.intersect && (
						(is.facA < 0.95 && is.facA > 0.05) ||
						(is.facB < 0.95 && is.facB > 0.05))) {
					// nextNodes[i].pos = is.point;
					nextNodes[i].vel = {0.f, 0.f};
					// TODO: use signedness! velocity that resolves
					// intersection shouldn't be erased
					// auto normal = tkn::rhs::lnormal(nytl::normalized(a.b - a.a));
					// nextNodes[i].vel -= dot(nodes_[i].vel, normal) * normal;
					break;
				}
			}
			*/

			// instead of intersection: use repulsion forces
			 /*
			for(auto j = 1u; j < i; ++j) {
				// auto& other = nextNodes[j];
				// auto diff = next.pos - other.pos;
				tkn::Segment2f seg {nextNodes[j-1].pos, nextNodes[j].pos};
				auto dist = distance(next.pos, seg);
				if(dist < 0.02) {
					nytl::Vec2f dir = nytl::normalized(next.pos - seg.a + next.pos - seg.b);
					if(dist < 0.01) {
						// auto dir = nytl::normalized(diff);
						next.vel -= std::max(dot(next.vel, -dir), 0.f) * -dir;
						// other.vel -= std::max(dot(other.vel, dir), 0.f) * dir;
						// nextNodes[j-1].vel -= std::max(dot(nextNodes[j-1].vel, dir), 0.f) * dir;
						// nextNodes[j].vel -= std::max(dot(nextNodes[j].vel, dir), 0.f) * dir;
					}

					// auto force = dt * nytl::normalized(diff);
					auto force = dt * dir;
					next.vel += force;
					// nextNodes[j-1].vel -= force;
					// nextNodes[j].vel -= force;
				}
			}
			*/
		}

		// "muscles"
		auto kc = appContext().keyboardContext();
		if(kc->pressed(ny::Keycode::k1)) {
			auto& a = nextNodes.front();
			auto& b = nextNodes.back();
			auto f = force(a, b, 0.01, 0.0, 0.0 * nodes_.size() * segLength);
			auto acc = (1 / mass) * f;
			a.vel += acc;
			b.vel -= acc;
		}

		// "follow mouse"
		if(mouseDown_) {
			nextNodes[0].pos = mpos_;
			nextNodes[0].vel = {0.f, 0.f};
		}

		// TODO: iterative, also consider real geometric intersections
		// and handle them somehow
		for(auto i = 0u; i < nextNodes.size(); ++i) {
			// for(auto j = 0u; j < nextNodes.size(); ++j) {
			for(auto j = 0u; j + 1 < i; ++j) {
				// if(i == j || j == 0) {
				// 	continue;
				// }

				auto& next = nextNodes[i];

				auto& other = nextNodes[j];
				auto diff = next.pos - other.pos;
				auto dist = length(diff);
				// tkn::Segment2f seg {nextNodes[j-1].pos, nextNodes[j].pos};
				// auto dist = distance(next.pos, seg);
				if(dist < 0.03) {
					// nytl::Vec2f dir = nytl::normalized(next.pos - seg.a + next.pos - seg.b);
					if(dist < 0.015) {
						auto dir = nytl::normalized(diff);
						next.vel -= std::max(dot(next.vel, -dir), 0.f) * -dir;
						other.vel -= std::max(dot(other.vel, dir), 0.f) * dir;
						// nextNodes[j-1].vel -= std::max(dot(nextNodes[j-1].vel, dir), 0.f) * dir;
						// nextNodes[j].vel -= std::max(dot(nextNodes[j].vel, dir), 0.f) * dir;
					}

					auto force = dt * nytl::normalized(diff);
					// auto force = dt * dir;
					next.vel += force;
					other.vel -= force;
					// nextNodes[j-1].vel -= force;
					// nextNodes[j].vel -= force;
				}
			}
		}

		nodes_ = nextNodes;

		App::scheduleRedraw();
	}

	void updateDevice() override {
		auto map = vertices_.memoryMap();
		auto span = map.span();
		for(auto& node : nodes_) {
			tkn::write(span, node.pos);
		}
		map.flush();
	}

	const char* name() const override { return "hair"; }
protected:
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	vpp::SubBuffer vertices_;
	std::vector<Node> nodes_;

	nytl::Vec2f mpos_;
	bool mouseDown_ {false};
};

int main(int argc, const char** argv) {
	HairApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

