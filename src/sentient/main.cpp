#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/bits.hpp>

#include <nytl/vec.hpp>
#include <nytl/math.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipelineInfo.hpp>

#include <shaders/sentient.sentient.frag.h>
#include <shaders/sentient.sentient.vert.h>

// generates the vertices to draw a circle with triangle strip, writing
// them directly into the given data buffer (which must be large enough)
// count must be an even number
void circleTriStrip(nytl::Span<std::byte>& buf, float radius,
		nytl::Vec2f center = {}, unsigned count = 64) {
	using nytl::constants::pi;
	doi::write(buf, center + radius * nytl::Vec2f {1.f, 0.f});
	for(auto i = 0u; i < (count - 2) / 2; ++i) {
		float a = 2 * pi * i / float(count);
		auto p1 = center + radius * nytl::Vec2f {std::cos(a), std::sin(a)};
		auto p2 = center + radius * nytl::Vec2f {std::cos(a), -std::sin(a)};
		doi::write(buf, p1);
		doi::write(buf, p2);
	}
	doi::write(buf, center - radius * nytl::Vec2f {1.f, 0.f});
}

class Bulb {
public:
	// constexpr auto pointCount =

public:
	void init(vpp::Device& dev, vk::RenderPass rp, vk::SampleCountBits samples) {
		// pipe layout
		vk::PushConstantRange pcrange {};
		pcrange.stageFlags = vk::ShaderStageBits::fragment;
		pcrange.offset = 0;
		pcrange.size = 64;
		pipeLayout_ = {dev, {}, {pcrange}};

		// pipe
		vpp::ShaderModule vert(dev, sentient_sentient_vert_data);
		vpp::ShaderModule frag(dev, sentient_sentient_frag_data);
		vpp::GraphicsPipelineInfo gpi {rp, pipeLayout_, {{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}, 0, samples};

		constexpr auto stride = sizeof(nytl::Vec2f);
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::vertex};

		// vertex position attribute
		vk::VertexInputAttributeDescription attributes[1];
		attributes[0].format = vk::Format::r32g32Sfloat;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 1u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleStrip;

		vk::Pipeline vkpipe;
		vk::createGraphicsPipelines(dev, {},
			1, gpi.info(), NULL, vkpipe);

		pipe_ = {dev, vkpipe};

		// vert buffer
	}

	void render(vk::CommandBuffer cb) {
		(void) cb;
	}

	void update(double) {
	}

	bool updateDevice() {
		return false;
	}

protected:
	vpp::SubBuffer circle_;
	std::array<vpp::SubBuffer, 3> tail_;

	// vpp::TrDs ds_;
	// vpp::TrDsLayout dsLayout_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
};

class CreaturesApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		((void) cb);
	}

	void update(double delta) override {
		App::update(delta);
	}
};

int main(int argc, const char** argv) {
	CreaturesApp app;
	if(!app.init({"creatures", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
