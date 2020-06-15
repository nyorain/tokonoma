#include <tkn/singlePassApp.hpp>
#include <tkn/shader.hpp>
#include <tkn/bits.hpp>
#include <tkn/util.hpp>
#include <argagg.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/descriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/vk.hpp>
#include <nytl/vecOps.hpp>
#include <vui/gui.hpp>
#include <vui/dat.hpp>
#include <dlg/dlg.hpp>
#include <random>

#include <shaders/ps.ps.frag.h>
#include <shaders/ps.ps.vert.h>

// NOTE: derived from the original particles dummy app
// to be a more general particle system

class ParticleSystem {
public:
	struct Particle {
		nytl::Vec2f position {};
		nytl::Vec2f velocity {};
		float lifetime {}; // how much time left
		float alpha {}; // how much time already lived
	};

	static constexpr auto compUboSize = 3 * sizeof(float);
	static constexpr auto gfxUboSize = 2 * sizeof(float);

public:
	// When one of the gfx params if changes, paramsChanged must be set to true
	float alpha {1.f};
	float pointSize {1.f};
	bool paramsChanged {true};

	// The effect to use (passed to compute shader); starting by 1
	uint32_t effect {1};

public:
	ParticleSystem() = default;
	void init(vpp::Device& dev, vk::RenderPass rp,
			vk::SampleCountBits samples, unsigned count) {
		dev_ = &dev;

		// gfx stuff
		auto gfxBindings = std::array {
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment),
		};

		gfxDsLayout_.init(dev, gfxBindings);
		gfxDs_ = {dev.descriptorAllocator(), gfxDsLayout_};
		gfxPipelineLayout_ = {dev, {{gfxDsLayout_.vkHandle()}}, {}};

		auto usage = nytl::Flags(vk::BufferUsageBits::uniformBuffer);
		auto mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		gfxUbo_ = {device().bufferAllocator(), gfxUboSize, usage, mem};

		vpp::ShaderModule vertShader(device(), ps_ps_vert_data);
		vpp::ShaderModule fragShader(device(), ps_ps_frag_data);
		vpp::GraphicsPipelineInfo gpi {rp, gfxPipelineLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples};

		constexpr auto stride = sizeof(Particle);
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex};

		// vertex position attribute
		vk::VertexInputAttributeDescription attributes[2];
		attributes[0].format = vk::Format::r32g32Sfloat;

		attributes[1].format = vk::Format::r32Sfloat;
		attributes[1].offset = sizeof(float)  * 5; // alpha
		attributes[1].location = 1;

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
		auto compBindings = std::array {
			vpp::descriptorBinding(
				vk::DescriptorType::storageBuffer,
				vk::ShaderStageBits::compute),
			vpp::descriptorBinding(
				vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::compute)
		};

		compDsLayout_.init(dev, compBindings);
		compDs_ = {dev.descriptorAllocator(), compDsLayout_};
		compPipelineLayout_ = {dev, {{compDsLayout_.vkHandle()}}, {}};

		if (!reloadShader()) { // init compute pipeline
			throw std::runtime_error("could not load shader, see errors");
		}

		// buffer
		auto bufSize = compUboSize;
		usage = vk::BufferUsageBits::uniformBuffer;
		mem = dev.memoryTypeBits(vk::MemoryPropertyBits::hostVisible);
		compUbo_ = {device().bufferAllocator(), bufSize, usage, mem};

		vpp::DescriptorSetUpdate update1(compDs_);

		// create & upload particles
		this->count(count, &update1);

		// write descriptor
		update1.uniform({{{compUbo_}}});

		vpp::DescriptorSetUpdate update2(gfxDs_);
		update2.uniform({{{gfxUbo_}}});

		vpp::apply({{{update1}, {update2}}});
		count_ = count;
	}

	void compute(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, compPipeline_);
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::compute,
			compPipelineLayout_, 0, {{compDs_.vkHandle()}}, {});
		vk::cmdDispatch(cb, count_ / 256, 1, 1);
	}

	void render(vk::CommandBuffer cb) {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfxPipeline_);
		vk::cmdBindVertexBuffers(cb, 0, {{particleBuffer_.buffer().vkHandle()}},
			{{particleBuffer_.offset()}});
		vk::cmdBindDescriptorSets(cb, vk::PipelineBindPoint::graphics,
			gfxPipelineLayout_, 0, {{gfxDs_.vkHandle()}}, {});
		vk::cmdDraw(cb, count_, 1, 0, 0);
	}

	void updateDevice(double delta) {
		time_ += delta;

		auto view = compUbo_.memoryMap();
		auto span = view.span();
		tkn::write(span, float(delta));
		tkn::write(span, time_);
		tkn::write(span, effect);

		if(paramsChanged) {
			auto view = gfxUbo_.memoryMap();
			auto span = view.span();
			tkn::write(span, alpha);
			tkn::write(span, pointSize);
			paramsChanged = false;
		}
	}

	// reloads the compute shader
	// when this returns true, the command buffer has to be rerecorded.
	// returning false signals an error
	// must only be called during updateDevice
	bool reloadShader() {
		auto mod = tkn::loadShader(device(), "ps/ps.comp");
		if (!mod) { // error will be shown; continue with old module
			return false;
		}

		vk::ComputePipelineCreateInfo info;
		info.layout = compPipelineLayout_;
		info.stage.module = *mod;
		info.stage.pName = "main";
		info.stage.stage = vk::ShaderStageBits::compute;

		vk::Pipeline vkpipe;
		vk::createComputePipelines(device(), {}, 1, info, nullptr, vkpipe);
		compPipeline_ = {device(), vkpipe};
		return true;
	}

	// must only be called during updateDevice and command buffer has
	// to be rerecorded
	void count(unsigned count, vpp::DescriptorSetUpdate* update = nullptr) {
		dlg_assert(count > 0);

		// next power of 2
		unsigned power = 1;
		while(power < count)
			power *= 2;
		count = power;

		// NOTE: could make this smart; only recreate if really needed,
		// also copy old particles over... some work though to keep track
		// of everything especially copying since deferred
		auto bufSize = sizeof(Particle) * count;
		auto mem = device().memoryTypeBits(vk::MemoryPropertyBits::deviceLocal);
		auto usage = vk::BufferUsageBits::vertexBuffer
			| vk::BufferUsageBits::storageBuffer
			| vk::BufferUsageBits::transferDst;
		particleBuffer_ = {device().bufferAllocator(), bufSize,
			usage, mem};
		count_ = count;

		std::optional<vpp::DescriptorSetUpdate> localUpdate {};
		if(!update) {
			localUpdate = {{compDs_}};
			update = &*localUpdate;
		}

		update->storage({{{particleBuffer_}}});
	}

	const vpp::Device& device() const {
		return *dev_;
	}

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
	float time_ {};
};

class ParticlesApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	bool init(const nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		rvgInit();

		// system
		auto initCount = 5000;
		system_.init(vkDevice(), renderPass(), samples(), initCount);

		// gui
		auto& panel = gui().create<vui::dat::Panel>(
			nytl::Vec2f {50.f, 0.f}, 300.f, 150.f);
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

		auto alpha = std::to_string(system_.alpha);
		auto pointSize = std::to_string(system_.pointSize);
		auto count = std::to_string(initCount);

		panel.create<vui::dat::Textfield>("alpha", alpha).
			textfield().onSubmit = tfChangeProp(system_.alpha);
		panel.create<vui::dat::Textfield>("pointSize", pointSize).
			textfield().onSubmit = tfChangeProp(system_.pointSize);
		panel.create<vui::dat::Textfield>("count", count).
			textfield().onSubmit = tfChangeProp(changeCount_);

		return true;
	}

	nytl::Vec2f attractorPos(const nytl::Vec2i pos) {
		using namespace nytl::vec::cw::operators;
		auto normed = pos / nytl::Vec2f(windowSize());
		return 2 * normed - nytl::Vec{1.f, 1.f};
	}

	void beforeRender(vk::CommandBuffer cb) override {
		system_.compute(cb);
	}

	void render(vk::CommandBuffer cb) override {
		system_.render(cb);
		gui().draw(cb);
	}

	void update(double delta) override {
		Base::update(delta);
		Base::scheduleRedraw();
		delta_ = delta;
	}

	void updateDevice() override {
		Base::updateDevice();
		system_.updateDevice(delta_);

		if(reload_) {
			system_.reloadShader();
			reload_ = false;
			Base::scheduleRerecord();
		}

		if(changeCount_) {
			system_.count(*changeCount_);
			changeCount_ = {};
			Base::scheduleRerecord();
		}
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(ev.pressed) {
			if(ev.keycode == swa_key_r) {
				reload_ = true;
			} else {
				// check if its a number
				unsigned num;
				if(ev.utf8 && tkn::stoi(ev.utf8, num)) {
					dlg_info("Effect: {}", num);
					system_.effect = num;
				} else {
					return false;
				}
			}
		} else {
			return false;
		}

		return true;
	}

	const char* name() const override { return "particle system"; }

protected:
	ParticleSystem system_;
	double delta_ {};

	// deferred input
	bool reload_ {};
	std::optional<unsigned> changeCount_ {};
};

int main(int argc, const char** argv) {
	return tkn::appMain<ParticlesApp>(argc, argv);
}

