#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/bits.hpp>
#include <argagg.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/pipelineInfo.hpp>
#include <vpp/descriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/vk.hpp>
#include <ny/mouseButton.hpp>
#include <nytl/vecOps.hpp>
#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <dlg/dlg.hpp>
#include <random>

#include <shaders/particle.comp.h>
#include <shaders/particle.frag.h>
#include <shaders/particle.vert.h>

constexpr auto compUboSize = (2 + 4 * 5) * 4;
constexpr auto gfxUboSize = 2 * 4;

class ParticleSystem {
public:
	// params and stuff
	float alpha {0.2f};
	float pointSize {1.f};
	bool paramsChanged {true};

public:
	ParticleSystem() = default;
	void init(vpp::Device& dev, vk::RenderPass rp,
			vk::SampleCountBits samples, unsigned count) {
		dev_ = &dev;

		// gfx stuff
		auto gfxBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment,
				0),
		};

		gfxDsLayout_ = {dev, gfxBindings};
		gfxDs_ = {dev.descriptorAllocator(), gfxDsLayout_};
		gfxPipelineLayout_ = {dev, {gfxDsLayout_}, {}};

		auto usage = nytl::Flags(vk::BufferUsageBits::uniformBuffer);
		auto mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		gfxUbo_ = {device().bufferAllocator(), gfxUboSize, usage, 0u, mem};

		vpp::ShaderModule vertShader(device(), particle_vert_data);
		vpp::ShaderModule fragShader(device(), particle_frag_data);
		vpp::GraphicsPipelineInfo gpi {rp, gfxPipelineLayout_, {{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}, 0, samples};

		constexpr auto stride = sizeof(float) * 4; // vec2 pos, velocity
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex};

		// vertex position attribute
		vk::VertexInputAttributeDescription attributes[2];
		attributes[0].format = vk::Format::r32g32Sfloat;

		attributes[1].format = vk::Format::r32g32Sfloat;
		attributes[1].location = 1;
		attributes[1].offset = sizeof(float) * 2;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 2u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;
		gpi.assembly.topology = vk::PrimitiveTopology::pointList;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(device(), {},
			1, gpi.info(), NULL, vkpipe);

		gfxPipeline_ = {device(), vkpipe};

		// compute stuff
		auto compBindings = {
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute, 0),
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute, 1)
		};

		compDsLayout_ = {dev, compBindings};
		compDs_ = {dev.descriptorAllocator(), compDsLayout_};
		compPipelineLayout_ = {dev, {compDsLayout_}, {}};

		vk::ComputePipelineCreateInfo cpi;
		vpp::ShaderModule compShader(device(), particle_comp_data);

		vk::ComputePipelineCreateInfo info;
		info.layout = compPipelineLayout_;
		info.stage.module = compShader;
		info.stage.pName = "main";
		info.stage.stage = vk::ShaderStageBits::compute;

		vk::createComputePipelines(device(), {}, 1, info, nullptr, vkpipe);
		compPipeline_ = {dev, vkpipe};

		// buffer
		struct Particle {
			nytl::Vec2f pos;
			nytl::Vec2f vel;
		};

		mem = 0xFFFFFFFF; // choose memory type here
		auto bits = dev.memoryTypeBits(vk::MemoryPropertyBits::deviceLocal);
		if(mem == 0xFFFFFFFF || !(bits & (1 << mem))) {
			mem = bits;
		}

		auto bufSize = sizeof(Particle) * count;
		usage = vk::BufferUsageBits::vertexBuffer
			| vk::BufferUsageBits::storageBuffer
			| vk::BufferUsageBits::transferDst;
		particleBuffer_ = {device().bufferAllocator(), bufSize,
			usage, 0u, mem};

		bufSize = compUboSize;
		usage = vk::BufferUsageBits::uniformBuffer;
		mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		compUbo_ = {device().bufferAllocator(), bufSize, usage, 0u, mem};

		// create & upload particles
		{
			constexpr auto distrFrom = -0.85f;
			constexpr auto distrTo = 0.85f;

			std::mt19937 rgen;
			rgen.seed(std::time(nullptr));
			std::uniform_real_distribution<float> distr(distrFrom, distrTo);

			std::vector<Particle> particles(count);
			for(auto& part : particles) {
				part.pos[0] = distr(rgen);
				part.pos[1] = distr(rgen);
				part.vel = {0.f, 0.f};
			}

			vpp::writeStaging430(particleBuffer_, vpp::rawSpan(particles));
		}

		// write descriptor
		{
			vpp::DescriptorSetUpdate update1(compDs_);
			update1.storage({{particleBuffer_.buffer(),
				particleBuffer_.offset(), particleBuffer_.size()}});
			update1.uniform({{compUbo_.buffer(), compUbo_.offset(),
				compUbo_.size()}});

			vpp::DescriptorSetUpdate update2(gfxDs_);
			update2.uniform({{gfxUbo_.buffer(), gfxUbo_.offset(),
				gfxUbo_.size()}});

			vpp::apply({update1, update2});
		}

		count_ = count;
	}

	void compute(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			compPipelineLayout_, 0, {compDs_}, {});
		vk::cmdDispatch(cb, count_ / 16, 1, 1);
	}

	void render(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipeline_);
		vk::cmdBindVertexBuffers(cb, 0, {particleBuffer_.buffer()},
			{particleBuffer_.offset()});
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gfxPipelineLayout_, 0, {gfxDs_}, {});
		vk::cmdDraw(cb, count_, 1, 0, 0);
	}

	void updateDevice(double delta, nytl::Span<nytl::Vec2f> attractors) {
		if(attractors.size() > 10) {
			 attractors = attractors.slice(0, 10);
		}

		auto view = compUbo_.memoryMap();
		auto span = view.span();

		for(auto p : attractors) {
			doi::write(span, p);
		}

		auto off = sizeof(nytl::Vec4f) * 5;
		span = view.span().slice(off, view.span().size() - off);
		doi::write(span, float(delta));
		doi::write(span, std::uint32_t(attractors.size()));

		if(paramsChanged) {
			auto view = gfxUbo_.memoryMap();
			auto span = view.span();
			doi::write(span, alpha);
			doi::write(span, pointSize);
		}
	}

	const vpp::Device& device() const { return *dev_; }

protected:
	vpp::Device* dev_ {};
	vpp::Pipeline gfxPipeline_;
	vpp::PipelineLayout gfxPipelineLayout_;

	vpp::Pipeline compPipeline_;
	vpp::PipelineLayout compPipelineLayout_;

	vpp::SubBuffer particleBuffer_;
	vpp::SubBuffer compUbo_;
	vpp::TrDsLayout compDsLayout_;
	vpp::TrDs compDs_;

	vpp::TrDsLayout gfxDsLayout_;
	vpp::TrDs gfxDs_;
	vpp::SubBuffer gfxUbo_;

	unsigned count_ {};
};

class ParticlesApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		// system
		auto& r = renderer();
		system_.init(vulkanDevice(), r.renderPass(), r.samples(), count_);

		// gui
		auto& panel = gui().create<vui::dat::Panel>(
			nytl::Vec2f {50.f, 0.f}, 300.f, 150.f);
		auto tfChangeProp = [&](auto& prop) {
			return [&](auto& tf) {
				try {
					prop = std::stof(tf.utf8());
					system_.paramsChanged = true;
				} catch(const std::exception& err) {
					dlg_error("Invalid float: {}", tf.utf8());
				}
			};
		};

		panel.create<vui::dat::Textfield>("alpha").textfield().onSubmit =
			tfChangeProp(system_.alpha);
		panel.create<vui::dat::Textfield>("pointSize").textfield().onSubmit =
			tfChangeProp(system_.pointSize);

		return true;
	}

	argagg::parser argParser() const override {
		auto parser = App::argParser();
		parser.definitions.push_back({
			"count",
			{"-c", "--count"},
			"Number of particles", 1
		});
		return parser;
	}

	nytl::Vec2f attractorPos(const nytl::Vec2i pos) {
		using namespace nytl::vec::cw::operators;
		auto normed = pos / nytl::Vec2f(window().size());
		return 2 * normed - nytl::Vec{1.f, 1.f};
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			attractors_.resize(ev.pressed);
			if(ev.pressed) {
				attractors_[0] = attractorPos(ev.position);
			}

			return true;
		}

		return false;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		if(!attractors_.empty()) {
			attractors_[0] = attractorPos(ev.position);
		}
	}

	bool handleArgs(const argagg::parser_results& result) override {
		if (!App::handleArgs(result)) {
			return false;
		}

		if(result["count"].count()) {
			count_ = result["count"].as<unsigned>();
		}

		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		system_.compute(cb);
	}

	void render(vk::CommandBuffer cb) override {
		system_.render(cb);
		gui().draw(cb);
	}

	void update(double delta) override {
		App::update(delta);
		App::scheduleRedraw();
		delta_ = delta;
	}

	void updateDevice() override {
		App::updateDevice();
		system_.updateDevice(delta_, attractors_);
	}

protected:
	ParticleSystem system_;
	unsigned count_ {100'000};
	double delta_ {};
	std::vector<nytl::Vec2f> attractors_ {};
};

int main(int argc, const char** argv) {
	ParticlesApp app;
	if(!app.init({"particles", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}

