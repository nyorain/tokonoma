// ideas:
// - play around with different attraction functions
// - allow the mouse cursor to be one particle and influence the whole
//   system like that (interesting to study the effect one individual particle
//   has on the others over time)

#include <tkn/app.hpp>
#include <tkn/window.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>
#include <vpp/pipeline.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <vui/dat.hpp>
#include <vui/gui.hpp>
#include <ny/key.hpp>
#include <dlg/dlg.hpp>
#include <cstring>
#include <random>

#include <shaders/pursuers.line.vert.h>
#include <shaders/pursuers.line.frag.h>

constexpr auto pointCount = 128; // needs to be updated in line.vert
constexpr auto particleCount = 2000;

struct Parameters {
	bool normalize; // whether to normalize distance
	float friction; // per 100ms

	struct {
		float constant;
		float invDist;
		float maxInvDist;
	} attraction;
};

Parameters p1 = {
	false,
	0.9,
	{
		2.f,
		2.f,
		100.f
	},
};

Parameters p2 = {
	true,
	0.3,
	{
		5.f,
		1.f,
		50.f
	},
};

class PursuerSystem {
public:
	Parameters params = p1;

public:
	PursuerSystem(vpp::Device& dev) {
		auto pcount = pointCount * particleCount;
		auto size = pcount * sizeof(nytl::Vec2f);
		buf_ = {dev.bufferAllocator(), size,
			vk::BufferUsageBits::vertexBuffer, dev.hostMemoryTypes()};

		// setup
		std::mt19937 rgen;
		rgen.seed(std::time(nullptr));
		std::uniform_real_distribution<float> posDistr(-1.f, 1.f);

		particles_.reserve(particleCount);
		points_.resize(pcount);
		for(auto i = 0u; i < particleCount; ++i) {
			auto pos = nytl::Vec2f {posDistr(rgen), posDistr(rgen)};
			float b = 0.2 + 0.3 * i / float(particleCount);
			auto col = nytl::Vec4f {0.2f, 0.9f - b, b, 1.f};
			particles_.push_back({pos, {}, col});
			for(auto j = 0u; j < pointCount; ++j) {
				points_[i * pointCount + j] = pos;
			}
		}

	}

	void update(double dt) {
		// shift all position by one
		auto size = (points_.size() - 1) * (sizeof(points_[0]));
		std::memmove(points_.data() + 1, points_.data(), size);

		auto follow = &particles_.back();
		auto i = 0u;
		for(auto& p : particles_) {
			p.pos += dt * p.vel;
			points_[i] = p.pos;
			p.vel *= std::pow(params.friction, dt * 100.f);

			auto d = follow->pos - p.pos;
			auto dd = dot(d, d);

			if(dd != 0.f) {
				auto fac = params.attraction.constant;
				fac += std::min(params.attraction.invDist / dd,
					params.attraction.maxInvDist);

				if(params.normalize) {
					normalize(d);
				}

				p.vel += dt * fac * d;

				// == ideas ==
				// add invSqrtDist attraction
				// fac += std::min(0.2f / std::sqrt(dd), 30.f);
			}

			follow = &p;
			i += pointCount;
		}
	}

	void updateDevice() {
		auto map = buf_.memoryMap();
		auto size = points_.size() * sizeof(points_[0]);
		std::memcpy(map.ptr(), points_.data(), size);
	}

	void render(vk::CommandBuffer cb, vk::PipelineLayout layout) {
		// could be done more efficiently if we ignore color i guess
		auto offset = 0u;
		for(auto& p : particles_) {
			auto c = p.color;
			vk::cmdPushConstants(cb, layout,
				vk::ShaderStageBits::fragment, 0u, sizeof(c), &c);
			vk::cmdBindVertexBuffers(cb, 0, 1, buf_.buffer(), offset);
			vk::cmdDraw(cb, pointCount, 1, 0, 0);
			offset += pointCount * sizeof(nytl::Vec2f);
		}
	}

protected:
	std::vector<nytl::Vec2f> points_;

	struct Particle {
		nytl::Vec2f pos {};
		nytl::Vec2f vel {};
		nytl::Vec4f color {};
	};

	std::vector<Particle> particles_;
	vpp::SubBuffer buf_;
};

class PursuersApp : public tkn::App {
public:
	bool init(const nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		auto& dev = vulkanDevice();

		// pipe
		auto range = vk::PushConstantRange {vk::ShaderStageBits::fragment,
			0u, sizeof(nytl::Vec4f)};
		particlePipeLayout_ = {dev, {}, {{range}}};

		auto lineVert = vpp::ShaderModule(dev, pursuers_line_vert_data);
		auto lineFrag = vpp::ShaderModule(dev, pursuers_line_frag_data);

		vpp::GraphicsPipelineInfo pipeInfo(renderPass(), particlePipeLayout_, {{{
				{lineVert, vk::ShaderStageBits::vertex},
				{lineFrag, vk::ShaderStageBits::fragment}
		}}}, 0u, samples());

		constexpr auto stride = sizeof(float) * 2;
		auto bufferBinding = vk::VertexInputBindingDescription {
			0, stride, vk::VertexInputRate::vertex
		};

		vk::VertexInputAttributeDescription attributes[1];
		attributes[0].format = vk::Format::r32g32Sfloat;

		pipeInfo.vertex.vertexBindingDescriptionCount = 1;
		pipeInfo.vertex.pVertexBindingDescriptions = &bufferBinding;
		pipeInfo.vertex.vertexAttributeDescriptionCount = 1;
		pipeInfo.vertex.pVertexAttributeDescriptions = attributes;

		pipeInfo.assembly.topology = vk::PrimitiveTopology::lineStrip;

		vk::Pipeline vkPipe;
		vk::createGraphicsPipelines(dev, {}, 1, pipeInfo.info(),
			nullptr, vkPipe);
		particlePipe_ = {dev, vkPipe};

		system_.emplace(dev);

		// gui
		using namespace vui::dat;
		panel_ = &gui().create<Panel>(nytl::Vec2f{50.f, 0.f}, 300.f, 150.f);

		auto createValueTextfield = [&](auto& at, auto name, float& value) {
			auto start = std::to_string(value);
			start.resize(4);
			auto& t = at.template create<Textfield>(name, start).textfield();
			t.onSubmit = [&, name](auto& tf) {
				try {
					value = std::stof(std::string(tf.utf8()));
				} catch(const std::exception& err) {
					dlg_error("Invalid float for {}: {}", name, tf.utf8());
					return;
				}
			};
		};

		auto& params = system_->params;
		params = p2;
		createValueTextfield(*panel_, "friction", params.friction);
		panel_->create<Button>("Load 1").onClick = [&]{
			params = p1;
		};

		panel_->create<Button>("Load 2").onClick = [&]{
			params = p2;
		};

		auto& folder = panel_->create<Folder>("attraction");
		auto& a = params.attraction;
		createValueTextfield(folder, "constant", a.constant);
		createValueTextfield(folder, "invDist", a.invDist);
		createValueTextfield(folder, "maxInvDist", a.maxInvDist);
		auto& cb = folder.create<Checkbox>("normalize").checkbox();
		cb.set(params.normalize);
		cb.onToggle = [&](const auto& c) {
			params.normalize = c.checked();
		};

		return true;
	}

	void updateDevice() override {
		App::updateDevice();
		system_->updateDevice();
	}

	void update(double dt) override {
		App::update(dt);

		if(!frame_) {
			return;
		}

		// frame_ = false;
		system_->update(dt);
		App::scheduleRedraw();
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(ev.pressed && ev.keycode == ny::Keycode::p) {
			frame_ = !frame_;
			return true;
		}

		return false;
	}

	void render(vk::CommandBuffer cb) override {
		App::render(cb);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, particlePipe_);
		system_->render(cb, particlePipeLayout_);
		gui().draw(cb);
	}

	const char* name() const override { return "pursuers"; }

protected:
	vpp::PipelineLayout particlePipeLayout_;
	vpp::Pipeline particlePipe_;
	std::optional<PursuerSystem> system_;
	vui::dat::Panel* panel_;
	bool frame_ {};
};

// main
int main(int argc, const char** argv) {
	PursuersApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
