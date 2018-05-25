#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipelineInfo.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <ny/key.hpp>
#include <dlg/dlg.hpp>
#include <cstring>
#include <random>

#include <shaders/line.vert.h>
#include <shaders/line.frag.h>

constexpr auto pointCount = 256;
class Particle {
public:
	Particle* follow;

public:
	Particle(vpp::Device& dev, Particle* xfollow, nytl::Vec2f pos,
			nytl::Vec4f color) : follow(xfollow), pos_(pos), color_(color) {

		points_.resize(pointCount, pos_);
		auto size = points_.size() * sizeof(points_[0]);
		buf_ = {dev.bufferAllocator(), size,
			vk::BufferUsageBits::vertexBuffer, 4u, dev.hostMemoryTypes()};
	}

	void update(double dt) {
		dlg_assert(follow);
		pos_ += dt * vel_;

		// slow down
		vel_ *= std::pow(0.9, dt * 100.f);

		// gravitation to last particle
		auto d = follow->pos() - pos_;
		auto dd = dot(d, d);

		if(dd != 0.f) {
			auto fac = 0.f;
			fac += std::min(1.f / dd, 100.f);
			// fac = std::min(5.f / std::sqrt(dd), 10.f);
			fac += 2.f;
			// normalize(d);
			vel_ += dt * fac * d;
		}

		points_.pop_back();
		points_.insert(points_.begin(), pos_);
	}

	void updateDevice() {
		auto map = buf_.memoryMap();
		auto size = points_.size() * sizeof(points_[0]);
		std::memcpy(map.ptr(), points_.data(), size);
	}

	nytl::Vec2f pos() const { return pos_; }
	nytl::Vec2f vel() const { return vel_; }
	nytl::Vec4f color() const { return color_; }
	const auto& buf() const { return buf_; }

protected:
	std::vector<nytl::Vec2f> points_;
	vpp::SubBuffer buf_;
	nytl::Vec2f pos_ {};
	nytl::Vec2f vel_ {};
	nytl::Vec4f color_ {};
};

class PursuersApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		auto& dev = vulkanDevice();

		// pipe
		auto range = vk::PushConstantRange {vk::ShaderStageBits::fragment,
			0u, sizeof(nytl::Vec4f)};
		particlePipeLayout_ = {dev, {}, {range}};

		auto lineVert = vpp::ShaderModule(dev, line_vert_data);
		auto lineFrag = vpp::ShaderModule(dev, line_frag_data);

		vpp::GraphicsPipelineInfo pipeInfo(renderer().renderPass(),
			particlePipeLayout_, vpp::ShaderProgram({
				{lineVert, vk::ShaderStageBits::vertex},
				{lineFrag, vk::ShaderStageBits::fragment}
		}), 0u, samples());

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

		// particles
		constexpr auto particleCount = 1000u;
		auto needed = particleCount * sizeof(nytl::Vec2f) * pointCount;
		dev.bufferAllocator().reserve(2 * needed,
			vk::BufferUsageBits::vertexBuffer, 4u, dev.hostMemoryTypes());

		std::mt19937 rgen;
		rgen.seed(std::time(nullptr));
		std::uniform_real_distribution<float> posDistr(-1.f, 1.f);

		Particle* last = nullptr;
		particles_.reserve(particleCount);
		for(auto i = 0u; i < particleCount; ++i) {
			auto pos = nytl::Vec2f {posDistr(rgen), posDistr(rgen)};
			float b = 0.2 + 0.3 * i / float(particleCount);
			auto col = nytl::Vec4f {0.2f, 0.9f - b, b, 1.f};
			particles_.emplace_back(dev, last, pos, col);
			last = &particles_.back();
		}

		particles_.front().follow = &particles_.back();
		return true;
	}

	bool updateDevice() override {
		auto ret = App::updateDevice();
		for(auto& p : particles_) {
			p.updateDevice();
		}

		return ret;
	}

	void update(double dt) override {
		App::update(dt);

		if(!frame_) {
			return;
		}

		// frame_ = false;
		for(auto& p : particles_) {
			p.update(dt);
		}
	}

	void key(const ny::KeyEvent& ev) override {
		if(ev.pressed && ev.keycode == ny::Keycode::n) {
			frame_ = !frame_;
		}
	}

	void render(vk::CommandBuffer cb) override {
		App::render(cb);
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, particlePipe_);
		for(auto& p : particles_) {
			auto c = p.color();
			vk::cmdPushConstants(cb, particlePipeLayout_,
				vk::ShaderStageBits::fragment, 0u, sizeof(c), &c);
			vk::cmdBindVertexBuffers(cb, 0, {p.buf().buffer()},
				{p.buf().offset()});
			vk::cmdDraw(cb, pointCount, 1, 0, 0);
		}
	}

protected:
	vpp::PipelineLayout particlePipeLayout_;
	vpp::Pipeline particlePipe_;
	std::vector<Particle> particles_;
	bool frame_ {};
};

// main
int main(int argc, const char** argv) {
	PursuersApp app;
	if(!app.init({"pendulum", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
