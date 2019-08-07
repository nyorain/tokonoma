

// generates the vertices to draw a circle with triangle strip, writing
// them directly into the given data buffer (which must be large enough)
// count must be an even number
void circleTriStrip(nytl::Span<std::byte>& buf, float radius,
		nytl::Vec2f center = {}, unsigned count = 64) {
	using nytl::constants::pi;
	tkn::write(buf, center + radius * nytl::Vec2f {1.f, 0.f});
	for(auto i = 0u; i < (count - 2) / 2; ++i) {
		float a = 2 * pi * i / float(count);
		auto p1 = center + radius * nytl::Vec2f {std::cos(a), std::sin(a)};
		auto p2 = center + radius * nytl::Vec2f {std::cos(a), -std::sin(a)};
		tkn::write(buf, p1);
		tkn::write(buf, p2);
	}
	tkn::write(buf, center - radius * nytl::Vec2f {1.f, 0.f});
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
		pipeLayout_ = {dev, {}, {{pcrange}}};

		// pipe
		vpp::ShaderModule vert(dev, sentient_sentient_vert_data);
		vpp::ShaderModule frag(dev, sentient_sentient_frag_data);
		vpp::GraphicsPipelineInfo gpi {rp, pipeLayout_, {{{
			{vert, vk::ShaderStageBits::vertex},
			{frag, vk::ShaderStageBits::fragment},
		}}}, 0, samples};

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

		pipe_ = {dev, gpi.info()};

		// vert buffer
		// TODO
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

