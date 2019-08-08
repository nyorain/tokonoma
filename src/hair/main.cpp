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
		nytl::Vec2f normal;
		float width;
		float mass;
	};

	static constexpr auto nodeCount = 16u;
	static constexpr auto ks1 = 30.f;
	static constexpr auto ks2 = 20.f;
	static constexpr auto ks3 = 20.f;
	static constexpr auto kd1 = 0.01f;
	static constexpr auto kd2 = 0.01f;
	static constexpr auto kd3 = 0.01f;
	static constexpr auto kdg = 5.f; // general dampening per second
	static constexpr auto segLength = 0.5f / nodeCount;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		// init logic
		auto pos = nytl::Vec2f{0.f, -0.5f};
		auto width = 0.1f;
		auto widthStep = 0.9 * width / (nodeCount);
		for(auto i = 0u; i < nodeCount; ++i) {
			auto& node = nodes_.emplace_back();
			node.pos = pos;
			node.vel = {0.f, 0.f};
			node.width = width;
			node.mass = 1.f * width;

			pos.y += segLength;
			width -= widthStep;
		}

		nodes_[0].width *= 2.f;
		nodes_[0].mass *= 2.f;

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
		auto kc = appContext().keyboardContext();

		auto force = [](auto& node1, auto& node2, float ks, float kd, float l) {
			auto diff = node2.pos - node1.pos;
			auto vdiff = node2.vel - node1.vel;
			auto ld = length(diff);
			diff *= 1 / ld;
			return (ks * (ld - l) + kd * dot(vdiff, diff)) * diff;
		};

		auto muscleForce = [&](const auto& node1, const auto& node2) {
			auto nl = node1;
			auto nr = node1;
			nl.pos += node1.width * node1.normal;
			nr.pos -= node1.width * node1.normal;

			auto ksa = 0.f;
			auto ksb = 0.f;

			auto ll = std::sqrt(segLength * segLength + node1.width * node1.width);
			auto lr = ll;
			// auto ll = nytl::length(nl.pos - node2.pos);
			// auto lr = nytl::length(nr.pos - node2.pos);
			// auto strength = 10.f * std::sqrt(segLength);
			auto strength = 5.f;
			// auto width = std::sqrt(node1.width); // node.width or 1?
			auto width = node1.width; // node.width or 1?
			// auto width = 1.f;
			if(kc->pressed(ny::Keycode::k1)) {
				// ll *= std::pow(0.1, node.width);
				// lr /= std::pow(0.1, node.width);
				ll *= 0.0;
				lr *= 2.0;
				ksa = strength * width;
				ksb = strength * width;
			}
			if(kc->pressed(ny::Keycode::k2)) {
				// lr *= std::pow(0.1, node.width);
				// ll /= std::pow(0.1, node.width);
				lr *= 0.0;
				ll *= 2.0;
				ksb = strength * width;
				ksa = strength * width;
			}

			return force(nl, node2, ksa, 0.f, ll) +
				force(nr, node2, ksb, 0.f, lr);
		};

		// compute normal for all nodes
		for(auto i = 0u; i < nodes_.size(); ++i) {
			auto& node = nodes_[i];

			// NOTE: not sure about tangent calculation/interpolation
			// normalize segments before addition or not?
			nytl::Vec2f tangent {0.f, 0.f};
			if(i > 0) {
				// tangent += 0.5 * nytl::normalized(nodes_[i].pos - nodes_[i - 1].pos);
				tangent += 0.5 * (nodes_[i].pos - nodes_[i - 1].pos);
			}
			if(i < nodes_.size() - 1) {
				// tangent += 0.5 * nytl::normalized(nodes_[i + 1].pos - nodes_[i].pos);
				tangent += 0.5 * (nodes_[i + 1].pos - nodes_[i].pos);
			}

			// node.normal = tkn::rhs::lnormal(tangent);
			node.normal = tkn::rhs::lnormal(nytl::normalized(tangent));
		}

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

			/*
			auto nl = node;
			auto nr = node;
			nl.pos += node.width * node.normal;
			nr.pos -= node.width * node.normal;

			auto ksa = 0.f;
			auto ksb = 0.f;

			auto ll = std::sqrt(segLength * segLength + node.width * node.width);
			auto lr = ll;
			auto strength = 30.f * segLength;
			// auto width = std::sqrt(node.width); // node.width or 1?
			auto width = 1.f;
			if(kc->pressed(ny::Keycode::k1)) {
				// ll *= std::pow(0.1, node.width);
				// lr /= std::pow(0.1, node.width);
				ll *= 0.5;
				lr *= 1.5;
				ksa = strength * width;
				ksb = strength * width;
			}
			if(kc->pressed(ny::Keycode::k2)) {
				// lr *= std::pow(0.1, node.width);
				// ll /= std::pow(0.1, node.width);
				lr *= 0.5;
				ll *= 1.5;
				ksb = strength * width;
				ksa = strength * width;
			}
			*/

			if(i > 0) {
				// auto nnl = nodes_[i - 1];
				// auto nnr = nodes_[i - 1];
				// nnl.pos += nnl.width * nnl.normal;
				// nnr.pos -= nnl.width * nnr.normal;

				// f += force(nl, nodes_[i-1], ksa, 0.f, ll);
				// f += force(nr, nodes_[i-1], ksb, 0.f, lr);
				f += muscleForce(node, nodes_[i - 1]);

				if(i > 1) {
					// f += force(nl, nodes_[i - 2], 0.5 * ksa, 0.f, 2 * ll);
					// f += force(nr, nodes_[i - 2], 0.5 * ksb, 0.f, 2 * lr);
					// f += 0.5 * muscleForce(node, nodes_[i - 2]);

					if(i > 2) {
						// f += force(nl, nodes_[i - 3], 0.3 * ksa, 0.f, 3 * ll);
						// f += force(nr, nodes_[i - 3], 0.3 * ksb, 0.f, 3 * lr);
						// f += 0.2 * muscleForce(node, nodes_[i - 3]);
					}
				}
			}

			if(i < nodes_.size() - 1) {
				// auto nnl = nodes_[i + 1];
				// auto nnr = nodes_[i + 1];
				// nnl.pos += nnl.width * nnl.normal;
				// nnr.pos -= nnl.width * nnr.normal;

				// f += force(nl, nodes_[i+1], ksa, 0.f, ll);
				// f += force(nr, nodes_[i+1], ksb, 0.f, lr);
				f -= muscleForce(nodes_[i + 1], node);

				if(i < nodes_.size() - 2) {
					// f += force(nl, nodes_[i + 2], 0.5 * ksa, 0.f, 2 * ll);
					// f += force(nr, nodes_[i + 2], 0.5 * ksb, 0.f, 2 * lr);
					// f -= 0.5 * muscleForce(nodes_[i + 2], node);

					if(i < nodes_.size() - 3) {
						// f += force(nl, nodes_[i + 3], 0.3 * ksa, 0.f, 3 * ll);
						// f += force(nr, nodes_[i + 3], 0.3 * ksb, 0.f, 3 * lr);
						// f -= 0.2 * muscleForce(nodes_[i + 3], node);
					}
				}
			}


			// f -= (1 - std::pow(kdg, dt)) * node.vel;
			// f -= kdg * node.vel;
			// auto tangent = tkn::rhs::lnormal(node.normal);

			// TODO: better special case for first AND last
			if(i == 0) {
				auto tangent = tkn::rhs::lnormal(node.normal);
				f -= 0.1 * kdg * dot(/*10 * node.mass */ node.width * node.vel, tangent) * tangent;
				// f -= kdg * (node.vel);
				// f -= kdg * (10 * node.mass * node.vel);
			}

			// f -= kdg * dot(10 * node.mass * node.vel, node.normal) * node.normal;
			// f -= kdg * dot(node.vel, node.normal) * node.normal;
			f -= kdg * dot(/*10 * node.mass * */ segLength * node.vel, node.normal) * node.normal;

			// auto a = (1 / (mass * node.width)) * f;
			auto a = (1 / (node.mass)) * f;

			// verlet-like integration
			auto& next = nextNodes[i];
			next.pos += dt * next.vel + 0.5 * dt * dt * a;
			next.vel += 0.5 * dt * a;
			// next.pos += dt * next.vel;
			// next.vel += dt * a;

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
		}

		// "muscles"
		// auto kc = appContext().keyboardContext();
		// if(kc->pressed(ny::Keycode::k1)) {
		// 	auto& a = nextNodes.front();
		// 	auto& b = nextNodes.back();
		// 	auto f = force(a, b, 0.01, 0.0, 0.0 * nodes_.size() * segLength);
		// 	auto acc = (1 / mass) * f;
		// 	a.vel += acc;
		// 	b.vel -= acc;
		// }

		// "follow mouse"
		if(mouseDown_) {
			nextNodes[0].pos = mpos_;
			nextNodes[0].vel = {0.f, 0.f};
		}

		/*
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
				if(dist < 0.02) {
					// nytl::Vec2f dir = nytl::normalized(next.pos - seg.a + next.pos - seg.b);
					if(dist < 0.01) {
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
		*/

		if(!mouseDown_) {
			// nextNodes[0].vel = {0.f, 0.f};
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

