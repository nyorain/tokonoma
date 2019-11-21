#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/bits.hpp>
#include <argagg.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/shader.hpp>
#include <vpp/descriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/vk.hpp>
#include <ny/mouseButton.hpp>
#include <nytl/vecOps.hpp>
#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <dlg/dlg.hpp>
#include <random>

#include <shaders/particles.particle.comp.h>
#include <shaders/particles.particle.frag.h>
#include <shaders/particles.particle.vert.h>

// velocity and position is always given in screen-space coords [-1, 1]

constexpr auto compUboSize = (6 + 2 + 4 * 5) * sizeof(float);
constexpr auto gfxUboSize = 2 * sizeof(float);

class ParticleSystem {
public:
	// params and stuff
	float alpha {0.1f};
	float pointSize {1.f};

	// set to true if one of above (gfx params) was changed
	bool paramsChanged {true};


	// it looks nicer if we add normalized (first component) and inv-linear
	// (second component) in addition to the physically correct
	// inv-squared (third component) distance as well.
	nytl::Vec3f attractionFactors {0.05, 0.1, 0.4};
	float friction {0.4f}; // lower -> more friction; 1/s

	float maxVel {5.f}; // screen space per second
	float distOff {0.05f};

	struct Particle {
		nytl::Vec2f pos;
		nytl::Vec2f vel;
	};

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
		gfxPipelineLayout_ = {dev, {{gfxDsLayout_.vkHandle()}}, {}};

		auto usage = nytl::Flags(vk::BufferUsageBits::uniformBuffer);
		auto mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		gfxUbo_ = {device().bufferAllocator(), gfxUboSize, usage, mem};

		vpp::ShaderModule vertShader(device(), particles_particle_vert_data);
		vpp::ShaderModule fragShader(device(), particles_particle_frag_data);
		vpp::GraphicsPipelineInfo gpi {rp, gfxPipelineLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples};

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
		compPipelineLayout_ = {dev, {{compDsLayout_.vkHandle()}}, {}};

		vk::ComputePipelineCreateInfo cpi;
		vpp::ShaderModule compShader(device(), particles_particle_comp_data);

		vk::ComputePipelineCreateInfo info;
		info.layout = compPipelineLayout_;
		info.stage.module = compShader;
		info.stage.pName = "main";
		info.stage.stage = vk::ShaderStageBits::compute;

		vk::createComputePipelines(device(), {}, 1, info, nullptr, vkpipe);
		compPipeline_ = {dev, vkpipe};

		// buffer
		mem = 0xFFFFFFFF; // choose memory type here
		auto bits = dev.memoryTypeBits(vk::MemoryPropertyBits::deviceLocal);
		if(mem == 0xFFFFFFFF || !(bits & (1 << mem))) {
			mem = bits;
		}

		auto bufSize = sizeof(Particle) * count;
		usage = vk::BufferUsageBits::vertexBuffer
			| vk::BufferUsageBits::storageBuffer
			| vk::BufferUsageBits::transferDst;
		particleBuffer_ = {device().bufferAllocator(), bufSize, usage, mem};

		bufSize = compUboSize;
		usage = vk::BufferUsageBits::uniformBuffer;
		mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		compUbo_ = {device().bufferAllocator(), bufSize, usage, mem};

		// create & upload particles
		count_ = count;
		reset();

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

			vpp::apply({{{update1}, {update2}}});
		}
	}

	void reset() {
		constexpr auto distrFrom = -0.85f;
		constexpr auto distrTo = 0.85f;
		auto& dev = *dev_;

		std::mt19937 rgen;
		rgen.seed(std::time(nullptr));
		std::uniform_real_distribution<float> distr(distrFrom, distrTo);

		std::vector<Particle> particles(count_);
		for(auto& part : particles) {
			part.pos[0] = distr(rgen);
			part.pos[1] = distr(rgen);
			part.vel = {0.f, 0.f};
		}

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		auto pspan = nytl::span(particles);
		auto stage = vpp::fillStaging(cb, particleBuffer_,
			nytl::as_bytes(pspan));

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));
	}

	void compute(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			compPipelineLayout_, 0, {{compDs_.vkHandle()}}, {});

		auto size = std::ceil(std::sqrt(count_ / 64.f));
		vk::cmdDispatch(cb, size, size, 1);
	}

	void render(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipeline_);
		vk::cmdBindVertexBuffers(cb, 0, {{particleBuffer_.buffer().vkHandle()}},
			{{particleBuffer_.offset()}});
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gfxPipelineLayout_, 0, {{gfxDs_.vkHandle()}}, {});
		vk::cmdDraw(cb, count_, 1, 0, 0);
	}

	void updateDevice(double delta, nytl::Span<nytl::Vec2f> attractors) {
		if(attractors.size() > 5) {
			 attractors = attractors.first(5);
		}

		auto view = compUbo_.memoryMap();
		auto span = view.span();

		for(auto p : attractors) {
			tkn::write(span, p);
		}

		span = view.span();
		auto off = sizeof(nytl::Vec4f) * 5;
		tkn::skip(span, off);

		tkn::write(span, attractionFactors);
		tkn::write(span, friction);
		tkn::write(span, maxVel);
		tkn::write(span, distOff);
		tkn::write(span, float(delta));
		tkn::write(span, std::uint32_t(attractors.size()));
		view.flush();

		if(paramsChanged) {
			auto view = gfxUbo_.memoryMap();
			auto span = view.span();
			tkn::write(span, alpha);
			tkn::write(span, pointSize);
			view.flush();
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

class ParticlesApp : public tkn::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		// system
		system_.init(vulkanDevice(), renderPass(), samples(), count_);

		// gui
		auto& panel = gui().create<vui::dat::Panel>(nytl::Vec2f {100.f, 0.f});
		auto tfChangeProp = [&](auto& prop) {
			return [&](auto& tf) {
				try {
					prop = std::stof(std::string(tf.utf8()));
					system_.paramsChanged = true;
				} catch(const std::exception& err) {
					dlg_error("Invalid float: {}", tf.utf8());
				}
			};
		};

		auto createProp = [&](auto name, auto& prop) {
			auto& tf = panel.create<vui::dat::Textfield>(name).textfield();
			tf.onSubmit = tfChangeProp(prop);
			auto str = std::to_string(prop);
			str.resize(4);
			tf.utf8(str);
		};

		createProp("Alpha", system_.alpha);
		createProp("Point Size", system_.pointSize);
		createProp("Friction", system_.friction);
		createProp("Max. Velocity", system_.maxVel);
		createProp("Distance Offset", system_.distOff);
		createProp("Attraction 0", system_.attractionFactors.x);
		createProp("Attraction 1", system_.attractionFactors.y);
		createProp("Attraction 2", system_.attractionFactors.z);

		panel.create<vui::dat::Button>("Reset").onClick = [&]{ reset_ = true; };

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

	// TODO: mouse and touch currently not usable at the same time
	// might crash application i guess
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

	bool touchBegin(const ny::TouchBeginEvent& ev) override {
		if(App::touchBegin(ev)) {
			return true;
		}

		auto pos = attractorPos(nytl::Vec2i(ev.pos));
		attractors_.push_back(pos);
		touchIDs_.push_back(ev.id);
		// dlg_trace("begin: {}", ev.id);
		return true;
	}

	bool touchEnd(const ny::TouchEndEvent& ev) override {
		if(App::touchEnd(ev)) {
			return true;
		}

		auto it = std::find(touchIDs_.begin(), touchIDs_.end(), ev.id);
		if(it != touchIDs_.end()) {
			// dlg_trace("erase: {}", ev.id);
			attractors_.erase(attractors_.begin() + (it - touchIDs_.begin()));
			touchIDs_.erase(it);
		} else {
			dlg_error("Invalid touch id");
		}

		return true;
	}

	void touchCancel(const ny::TouchCancelEvent&) override {
		dlg_trace("cancel");
		attractors_.clear();
		touchIDs_.clear();
	}

	void touchUpdate(const ny::TouchUpdateEvent& ev) override {
		App::touchUpdate(ev);

		auto pos = attractorPos(nytl::Vec2i(ev.pos));
		auto it = std::find(touchIDs_.begin(), touchIDs_.end(), ev.id);
		if(it != touchIDs_.end()) {
			attractors_[it - touchIDs_.begin()] = pos;
		} else {
			dlg_error("Invalid touch id");
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

		if(reset_) {
			reset_ = false;
			system_.reset();
		}
	}

	const char* name() const override { return "particles"; }

protected:
	ParticleSystem system_;
	unsigned count_ {500'000};
	double delta_ {};
	bool reset_ {};
	std::vector<nytl::Vec2f> attractors_ {};
	std::vector<unsigned> touchIDs_ {};
};

int main(int argc, const char** argv) {
	ParticlesApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

