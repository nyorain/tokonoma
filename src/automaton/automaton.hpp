#pragma once

#include <vpp/fwd.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vkpp/structs.hpp>
#include <nytl/vec.hpp>
#include <vui/dat.hpp>
#include <nytl/mat.hpp>

// General cellular automaton abstraction
// NOTE: in retrospection it's probably a bad idea trying to abstract
// something like this since its lot of work an still not as general
// as needed in most cases.
class Automaton {
public:
	enum class GridType {
		quad,
		hex,
	};

	enum class BufferMode {
		single,
		doubled
	};

	enum class ResizeMode {
		scaleLinear,
		scaleNearest,
		clear
	};

public:
	virtual ~Automaton() = default;

	virtual void worldClick(std::optional<nytl::Vec2f> pos);
	virtual void click(std::optional<nytl::Vec2ui> pos);
	virtual void display(vui::dat::Folder&);

	virtual void compute(vk::CommandBuffer);
	virtual void render(vk::CommandBuffer);
	virtual void resize(nytl::Vec2ui);
	virtual void transform(nytl::Mat4f t) { transform_ = t; }
	virtual void update(double) {}
	virtual std::pair<bool, vk::Semaphore> updateDevice();
	virtual void hexLines(bool);

	auto hexLines() const { return hex_.lines; }
	const auto& size() const { return size_; }
	const auto& img() const { return img_; }
	GridType gridType() const { return gridType_; }

	vpp::Device& device() { return *dev_; }
	const vpp::Device& device() const { return *dev_; }

protected:
	struct Fill {
		vk::Offset3D offset;
		vk::Extent3D size;
		std::vector<std::byte> data;
		vpp::SubBuffer stage {};
	};

	struct Retrieve {
		vk::Offset3D offset;
		vk::Extent3D size;
		vpp::SubBuffer* dst;
	};

	Automaton() = default;
	void init(vpp::Device& dev,
		vk::RenderPass, nytl::Span<const std::uint32_t> compShader,
		nytl::Vec2ui size, BufferMode buffer, vk::Format,
		nytl::Span<const std::uint32_t> vert = {},
		nytl::Span<const std::uint32_t> frag = {},
		std::optional<unsigned> count = {},
		GridType grid = GridType::quad,
		ResizeMode = ResizeMode::scaleNearest);

	void rerecord() { rerecord_ = true; }
	void gridType(GridType);
	void resizeMode(ResizeMode);
	void dispatchCount(std::optional<unsigned>);
	void set(nytl::Vec2ui pos, std::vector<std::byte> data);
	void fill(Fill);
	vk::CommandBuffer getRecording();

	void retrieve(Retrieve);
	void get(nytl::Vec2ui pos, vpp::SubBuffer* dst);

	virtual void initCompPipe(nytl::Span<const std::uint32_t> shader);
	virtual void initGfxPipe(vk::RenderPass,
		nytl::Span<const std::uint32_t> vert,
		nytl::Span<const std::uint32_t> frag);
	virtual void initImages();
	virtual void initLayouts();
	virtual void initSampler();
	virtual void initBuffers(unsigned additionalGfxSize = 0u);

	virtual void compDsUpdate(vpp::DescriptorSetUpdate&);
	virtual void compDsLayout(std::vector<vk::DescriptorSetLayoutBinding>&);
	virtual void compPipeLayout(
		std::vector<vk::DescriptorSetLayout>&,
		std::vector<vk::PushConstantRange>&);

	virtual void gfxDsUpdate(vpp::DescriptorSetUpdate&);
	virtual void gfxDsLayout(std::vector<vk::DescriptorSetLayoutBinding>&);
	virtual void gfxPipeLayout(
		std::vector<vk::DescriptorSetLayout>&,
		std::vector<vk::PushConstantRange>&);
	virtual void writeGfxData(nytl::Span<std::byte>& data);

private:
	vpp::Device* dev_;
	vpp::ViewableImage img_; // compute shader writes this, rendering reads
	vpp::ViewableImage imgBack_; // potentially unused back buffer
	vpp::ViewableImage imgOld_; // for resizing, blitting

	vpp::TrDsLayout compDsLayout_;
	vpp::PipelineLayout compPipelineLayout_;
	vpp::Pipeline compPipeline_;

	vpp::TrDs compDs_;
	bool rerecord_ {};
	vpp::Semaphore uploadSemaphore_ {};
	vpp::CommandBuffer uploadCb_ {};
	bool cbUsed_ {};

	vk::Format format_;
	GridType gridType_;
	ResizeMode resizeMode_;
	BufferMode bufferMode_;
	std::optional<unsigned> dispatchCount_;

	nytl::Vec2ui size_;
	std::optional<nytl::Vec2ui> resize_ {};

	std::vector<Fill> fill_;
	std::vector<Fill> oldFill_;
	std::vector<Retrieve> retrieve_;

	// render
	vpp::SubBuffer gfxUbo_;
	vpp::Sampler sampler_;
	vpp::TrDsLayout gfxDsLayout_;
	vpp::PipelineLayout gfxPipeLayout_;
	vpp::Pipeline gfxPipe_;
	vpp::Pipeline gfxPipeLines_;
	vpp::TrDs gfxDs_;
	std::optional<nytl::Mat4f> transform_;

	struct {
		bool lines {};
		float radius {};
		nytl::Vec2f off {};
	} hex_;
};
