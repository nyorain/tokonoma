#include <tkn/singlePassApp.hpp>
#include <tkn/shader.hpp>
#include <tkn/window.hpp>
#include <tkn/qcamera.hpp>
#include <tkn/timeWidget.hpp>
#include <tkn/bits.hpp>
#include <tkn/scene/shape.hpp>
#include <tkn/render.hpp>
#include <tkn/types.hpp>
#include <vpp/vk.hpp>
#include <vpp/shader.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/queue.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>

// TODO: maybe create separate application? that share a "terrain" class?
// - create custom renderpass with 2 subpasses
//   the second subpass gets the depth buffer as input attachment
// - in second subpass: render atmosphere (ball) with scattering shader
// - add sun shadow mapping
//   use that for scattering
//   but instead of using the whole frustum, shadow map only a part
//   around the camera (within the frustum). Make sure the transition
//   is at least linear though
//   	- check if global illumination can be implemented
// - add scattering and subdiv parameters to ui
// - add skybox with custom universe shader
// - add third subpass (that gets previous color as input) for tonemapping
// - add textures (and normal maps) to terrain
//   tile them dynamically, see iq's blogpost
// - make terrain generation more interesting. Deserts, seas, oceans,
//   lakes, rivers, etc. Maybe do some heavy pre-computations
//   modeling planet plates?
//   	- (way) later: don't model planets as surfaces anymore but
//   	  allow more general 3D noise patterns. Not sure how to
//   	  render yet. I mean raymarching could work?
//   	  maybe we can use raymarching to generate/subdivide
//   	  primitives but then render those normally?
// - (way later) add foliage: dynamically add grass (or at low lod approx it
//   via noise in the texture) and maybe trees?
// - TAA would be really useful with scattering

using namespace tkn::types;

class SubdApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	using Base::init;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		rvgInit();
		windowTransform_ = {rvgContext()};

		timeWidget_ = {rvgContext(), defaultFont()};
		timeWidget_.reset();
		timeWidget_.addTiming("update");
		timeWidget_.addTiming("build indirect");
		timeWidget_.addTiming("copy");
		timeWidget_.addTiming("draw");
		timeWidget_.complete();

		auto& dev = vkDevice();

		// camera/scene ubo
		auto uboSize = 2 * sizeof(nytl::Mat4f) + sizeof(nytl::Vec3f) + sizeof(u32);
		ubo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer,
			dev.hostMemoryTypes()};

		// indirect dispatch buffer
		comp_.dispatch = {dev.bufferAllocator(),
			sizeof(vk::DrawIndirectCommand) + sizeof(vk::DispatchIndirectCommand),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::indirectBuffer |
				vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};

		// vertices/indices
		auto planet = tkn::Sphere {{0.f, 0.f, 0.f}, {1.f, 1.f, 1.f}};
		auto shape = tkn::generateUV(planet, 4, 8);
		std::vector<nytl::Vec4f> vverts;
		for(auto& v : shape.positions) vverts.emplace_back(v);
		auto verts = tkn::bytes(vverts);
		auto inds = tkn::bytes(shape.indices);

		vertices_ = {dev.bufferAllocator(), verts.size(),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};
		indices_ = {dev.bufferAllocator(), inds.size(),
			vk::BufferUsageBits::storageBuffer |
				vk::BufferUsageBits::transferDst,
			dev.deviceMemoryTypes()};

		// key buffers
		// TODO: allow buffers to grow dynamically
		// condition can simply be checked. Modify compute shaders
		// to check for out-of-bounds
		auto triCount = 10 * 1024 * 1024;
		auto bufSize = triCount * sizeof(nytl::Vec2u32);
		auto usage = vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::storageBuffer |
			vk::BufferUsageBits::transferSrc |
			vk::BufferUsageBits::transferDst;

		keys0_ = {dev.bufferAllocator(), bufSize + 8, usage, dev.deviceMemoryTypes()};
		keys1_ = {dev.bufferAllocator(), bufSize + 8, usage, dev.deviceMemoryTypes()};
		keysCulled_ = {dev.bufferAllocator(), bufSize + 8, usage, dev.deviceMemoryTypes()};

		// write initial data to buffers
		tkn::DynamicBuffer data0;

		dlg_assert(shape.indices.size() % 3 == 0);
		auto numTris = shape.indices.size() / 3;
		write(data0, u32(numTris)); // counter
		write(data0, 0.f); // padding

		for(auto i = 0u; i < numTris; ++i) {
			write(data0, nytl::Vec2u32 {1, i});
		}

		struct {
			vk::DrawIndirectCommand draw;
			vk::DispatchIndirectCommand dispatch;
		} cmds {};

		cmds.draw.firstInstance = 0;
		cmds.draw.firstVertex = 0;
		cmds.draw.instanceCount = numTris;
		cmds.draw.vertexCount = 3; // triangle (list)
		cmds.dispatch.x = numTris;
		cmds.dispatch.y = 1;
		cmds.dispatch.z = 1;

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		auto stage0 = vpp::fillStaging(cb, keys0_, data0.buffer);
		auto stage1 = vpp::fillStaging(cb, comp_.dispatch, tkn::bytes(cmds));

		auto stage3 = vpp::fillStaging(cb, vertices_, verts);
		auto stage4 = vpp::fillStaging(cb, indices_, inds);

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// init other stuff
		initGfx();
		initUpdateComp();
		initIndirectComp();

		if(!createPipes()) {
			return false;
		}

		return true;
	}

	void initGfx() {
		auto& dev = vkDevice();
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer),
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer),
		};

		gfx_.dsLayout = {dev, bindings};
		gfx_.pipeLayout = {dev, {{gfx_.dsLayout}}, {}};

		gfx_.ds = {dev.descriptorAllocator(), gfx_.dsLayout};
		vpp::DescriptorSetUpdate dsu(gfx_.ds);
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{vertices_}}});
		dsu.storage({{{indices_}}});
	}

	void initUpdateComp() {
		auto& dev = vkDevice();
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer), // read
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer), // write
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer), // camera
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer), // verts
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer), // inds
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer), // culled
		};

		comp_.update.dsLayout = {dev, bindings};
		comp_.update.pipeLayout = {dev, {{comp_.update.dsLayout}}, {}};

		comp_.update.ds = {dev.descriptorAllocator(), comp_.update.dsLayout};
		vpp::DescriptorSetUpdate dsu(comp_.update.ds);
		dsu.storage({{{keys0_}}});
		dsu.storage({{{keys1_}}});
		dsu.uniform({{{ubo_}}});
		dsu.storage({{{vertices_}}});
		dsu.storage({{{indices_}}});
		dsu.storage({{{keysCulled_}}});
	}

	void initIndirectComp() {
		auto& dev = vkDevice();
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer), // dispatch buf
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer), // counter keys1
			vpp::descriptorBinding(vk::DescriptorType::storageBuffer), // counter keysCulled
		};

		comp_.indirect.dsLayout = {dev, bindings};
		comp_.indirect.pipeLayout = {dev, {{comp_.indirect.dsLayout}}, {}};

		comp_.indirect.ds = {dev.descriptorAllocator(), comp_.indirect.dsLayout};
		vpp::DescriptorSetUpdate dsu(comp_.indirect.ds);
		dsu.storage({{{comp_.dispatch}}});
		dsu.storage({{{keys1_}}});
		dsu.storage({{{keysCulled_}}});
	}

	bool createPipes() {
		auto& dev = vkDevice();

		bool failed = false;
		auto load = [&](auto name, const char* args = "") {
			auto pmod = tkn::loadShader(dev, name, args);
			if(!pmod) {
				failed = true;
				return vpp::ShaderModule {};
			}

			return std::move(*pmod);
		};

		auto modelVert = load("subd/model.vert");
		auto modelFrag = load("subd/model.frag");
		auto updateComp = load("subd/update.comp");
		auto indirectComp = load("subd/dispatch.comp");

		auto wireVert = load("subd/model.vert", "-DNO_TRANSFORM=1");
		auto wireFrag = load("tess/normal.frag"); // TODO: dep on other project
		auto wireGeom = load("subd/model.geom");

		if(failed) {
			return false;
		}

		// compute
		vk::ComputePipelineCreateInfo cpiUpdate;
		cpiUpdate.layout = comp_.update.pipeLayout;
		cpiUpdate.stage.module = updateComp;
		cpiUpdate.stage.pName = "main";
		cpiUpdate.stage.stage = vk::ShaderStageBits::compute;

		comp_.update.pipe = {dev, cpiUpdate};

		vk::ComputePipelineCreateInfo cpiIndirect;
		cpiIndirect.layout = comp_.indirect.pipeLayout;
		cpiIndirect.stage.module = indirectComp;
		cpiIndirect.stage.pName = "main";
		cpiIndirect.stage.stage = vk::ShaderStageBits::compute;

		comp_.indirect.pipe = {dev, cpiIndirect};

		// gfx
		vpp::GraphicsPipelineInfo gpi(renderPass(), gfx_.pipeLayout, {{{
			{modelVert, vk::ShaderStageBits::vertex},
			{modelFrag, vk::ShaderStageBits::fragment},
		}}});

		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;
		gpi.rasterization.cullMode = vk::CullModeBits::back;
		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		gpi.rasterization.polygonMode = vk::PolygonMode::fill;

		constexpr auto stride = sizeof(nytl::Vec2u32); // uvec2
		vk::VertexInputBindingDescription bufferBinding {
			0, stride, vk::VertexInputRate::instance};

		vk::VertexInputAttributeDescription attributes[1];
		attributes[0].format = vk::Format::r32g32Uint;

		gpi.vertex.pVertexAttributeDescriptions = attributes;
		gpi.vertex.vertexAttributeDescriptionCount = 1u;
		gpi.vertex.pVertexBindingDescriptions = &bufferBinding;
		gpi.vertex.vertexBindingDescriptionCount = 1u;

		gfx_.pipe = {vkDevice(), gpi.info()};

		// wirte pipe
		gpi.program = {{{
			{wireVert, vk::ShaderStageBits::vertex},
			{wireGeom, vk::ShaderStageBits::geometry},
			{wireFrag, vk::ShaderStageBits::fragment},
		}}};
		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = false;
		gpi.rasterization.polygonMode = vk::PolygonMode::line;
		gfx_.wirePipe = {vkDevice(), gpi.info()};

		return true;
	}

	void beforeRender(vk::CommandBuffer cb) override {
		timeWidget_.start(cb);

		// reset counter in dst buffer to 0
		vk::cmdFillBuffer(cb, keys1_.buffer(), keys1_.offset(), 4, 0);
		vk::cmdFillBuffer(cb, keysCulled_.buffer(), keysCulled_.offset(), 4, 0);

		// make sure the reset is visible
		vk::BufferMemoryBarrier barrier1;
		barrier1.buffer = keys1_.buffer();
		barrier1.offset = keys1_.offset();
		barrier1.size = 4u;
		barrier1.srcAccessMask = vk::AccessBits::transferWrite;
		barrier1.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;

		vk::BufferMemoryBarrier barrierCulled;
		barrierCulled.buffer = keysCulled_.buffer();
		barrierCulled.offset = keysCulled_.offset();
		barrierCulled.size = keysCulled_.size();
		barrierCulled.srcAccessMask = vk::AccessBits::transferWrite;
		barrierCulled.dstAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;

		// run update pipeline
		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::transfer,
			vk::PipelineStageBits::computeShader, {}, {},
			{{barrier1, barrierCulled}}, {});

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, comp_.update.pipe);
		tkn::cmdBindComputeDescriptors(cb, comp_.update.pipeLayout, 0, {comp_.update.ds});
		vk::cmdDispatchIndirect(cb, comp_.dispatch.buffer(),
			comp_.dispatch.offset() + sizeof(vk::DrawIndirectCommand));
		timeWidget_.addTimestamp(cb);

		// make sure updates in keys1_ is visible
		vk::BufferMemoryBarrier barrier0;
		barrier0.buffer = keys0_.buffer();
		barrier0.offset = keys0_.offset();
		barrier0.size = keys0_.size();
		barrier0.srcAccessMask = vk::AccessBits::shaderRead;
		barrier0.dstAccessMask = vk::AccessBits::transferWrite;

		barrier1.size = keys1_.size();
		barrier1.srcAccessMask = vk::AccessBits::shaderRead |
			vk::AccessBits::shaderWrite;
		barrier1.dstAccessMask = vk::AccessBits::transferRead |
			vk::AccessBits::shaderRead;

		barrierCulled.srcAccessMask = vk::AccessBits::shaderWrite;
		barrierCulled.dstAccessMask = vk::AccessBits::shaderRead;

		vk::cmdPipelineBarrier(cb, vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::transfer |
				vk::PipelineStageBits::vertexInput |
				vk::PipelineStageBits::computeShader,
			{}, {}, {{barrier0, barrier1, barrierCulled}}, {});

		// run indirect pipe to build commands
		// TODO: probably easier/better to do this with a simple one-word
		// buffer copy...
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, comp_.indirect.pipe);
		tkn::cmdBindComputeDescriptors(cb, comp_.indirect.pipeLayout, 0,
			{comp_.indirect.ds});
		vk::cmdDispatch(cb, 1, 1, 1);
		timeWidget_.addTimestamp(cb);

		// copy from keys1_ (the new triangles) to keys0_ (which are
		// used for drawing and in the next update step).
		// we could alternatively use ping-ponging and do 2 steps per
		// frame or just use 2 completely independent command buffers.
		// May be more efficient but harder to sync.
		auto keys1 = vpp::BufferSpan(keys1_.buffer(), keys1_.size(),
			keys1_.offset());
		tkn::cmdCopyBuffer(cb, keys1, keys0_);

		barrier0.srcAccessMask = vk::AccessBits::transferWrite;
		barrier0.dstAccessMask = vk::AccessBits::shaderRead;

		barrier1.srcAccessMask = vk::AccessBits::transferRead;
		barrier1.dstAccessMask = vk::AccessBits::shaderWrite;

		vk::BufferMemoryBarrier barrierIndirect;
		barrierIndirect.buffer = comp_.dispatch.buffer();
		barrierIndirect.offset = comp_.dispatch.offset();
		barrierIndirect.size = comp_.dispatch.size();
		barrierIndirect.srcAccessMask = vk::AccessBits::shaderWrite;
		barrierIndirect.dstAccessMask = vk::AccessBits::indirectCommandRead;

		// make sure the copy is visible for drawing (and the next step)
		vk::cmdPipelineBarrier(cb,
			vk::PipelineStageBits::transfer |
				vk::PipelineStageBits::computeShader,
			vk::PipelineStageBits::vertexShader |
				vk::PipelineStageBits::drawIndirect |
				vk::PipelineStageBits::computeShader,
			{}, {}, {{barrier0, barrier1, barrierIndirect}}, {});
		timeWidget_.addTimestamp(cb);
	}

	void render(vk::CommandBuffer cb) override {
		tkn::cmdBindGraphicsDescriptors(cb, gfx_.pipeLayout, 0, {gfx_.ds});
		vk::cmdBindVertexBuffers(cb, 0, {{keysCulled_.buffer()}},
			{{keysCulled_.offset() + 8}}); // skip counter and padding

		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfx_.pipe);
		vk::cmdDrawIndirect(cb, comp_.dispatch.buffer(), comp_.dispatch.offset(), 1, 0);

		// vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, gfx_.wirePipe);
		// vk::cmdDrawIndirect(cb, comp_.dispatch.buffer(), comp_.dispatch.offset(), 1, 0);

		rvgContext().bindDefaults(cb);
		windowTransform_.bind(cb);

		timeWidget_.draw(cb);
		timeWidget_.addTimestamp(cb);
		timeWidget_.finish(cb);
	}

	void update(double dt) override {
		Base::update(dt);

		// tkn::checkMovement(camera_, *kc, dt);
		tkn::QuatCameraMovementSwa movement {};
		movement.fastMult = 5.f;
		movement.slowMult = 0.05f;
		checkMovement(camera_, swaDisplay(), dt, movement);

		if(swa_display_key_pressed(swaDisplay(), swa_key_z)) {
			rollVel_ -= 0.1 * dt;
		}

		if(swa_display_key_pressed(swaDisplay(), swa_key_x)) {
			rollVel_ += 0.1 * dt;
		}

		if(rotateView_) {
			auto sign = [](auto f) { return f > 0.f ? 1.f : -1.f; };
			auto delta = mpos_ - mposStart_;
			auto yaw = 4 * dt * sign(delta.x) * std::pow(std::abs(delta.x), 1);
			auto pitch = 4 * dt * sign(delta.y) * std::pow(std::abs(delta.y), 1);
			tkn::rotateView(camera_, yaw, pitch, 0.f);
		}

		if(rollVel_ > 0.001) {
			rollVel_ *= std::pow(0.001, dt);
			tkn::rotateView(camera_, 0.f, 0.f, rollVel_);
		}

		if(camera_.update) {
			Base::scheduleRedraw();
		}
	}

	void updateDevice() override {
		Base::updateDevice();
		if(update_) {
			updateStep_ = true;
		}

		timeWidget_.updateDevice();

		// update scene ubo
		if(camera_.update) {
			camera_.update = false;

			auto fov = 0.35 * nytl::constants::pi;
			auto aspect = float(windowSize().x) / windowSize().y;
			auto near = 0.001f;
			auto far = 30.f;

			auto map = ubo_.memoryMap();
			auto span = map.span();
			auto V = viewMatrix(camera_);
			auto P = tkn::perspective3RH<float>(fov, aspect, near, far);
			auto VP = P * V;
			if(updateStep_) {
				frozenVP_ = VP;
			}
			tkn::write(span, VP);
			tkn::write(span, camera_.pos);
			tkn::write(span, u32(updateStep_));
			tkn::write(span, frozenVP_);
			map.flush();
		}

		if(reload_) {
			createPipes();
			reload_ = false;
			Base::scheduleRerecord();
		}

		updateStep_ = false;
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		using namespace nytl::vec::cw::operators;
		mpos_ = nytl::Vec2f{float(ev.x), float(ev.y)} / windowSize();
	}

	bool mouseWheel(float dx, float dy) override {
		return Base::mouseWheel(dx, dy);
	}

	bool mouseButton(const swa_mouse_button_event& ev) override {
		if(Base::mouseButton(ev)) {
			return true;
		}

		using namespace nytl::vec::cw::operators;
		auto mpos = nytl::Vec2f{float(ev.x), float(ev.y)} / windowSize();
		if(ev.button == swa_mouse_button_left) {
			rotateView_ = ev.pressed;
			mposStart_ = mpos;
			return true;
		}

		return false;
	}

	bool key(const swa_key_event& ev) override {
		if(Base::key(ev)) {
			return true;
		}

		if(!ev.pressed) {
			return false;
		}

		if(ev.keycode == swa_key_r) {
			reload_ = true;
			Base::scheduleRedraw();
			return true;
		} else if(ev.keycode == swa_key_o) { // update step
			updateStep_ = true;
			dlg_info("Doing one update step next frame");
			return true;
		} else if(ev.keycode == swa_key_p) { // constant updating
			update_ = !update_;
			dlg_info("update: {}", update_);
			return true;
		}

		return false;
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		// camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;

		// update window transform
		auto s = nytl::Vec{ 2.f / w, 2.f / h, 1};
		auto transform = nytl::identity<4, float>();
		tkn::scale(transform, s);
		tkn::translate(transform, nytl::Vec3f {-1, -1, 0});
		windowTransform_.matrix(transform);

		// update time widget position
		auto pos = nytl::Vec2ui{w, h};
		pos.x -= 240;
		pos.y = 10;
		timeWidget_.move(nytl::Vec2f(pos));
	}

	const char* name() const override { return "subd"; }
	bool needsDepth() const override { return true; }

protected:
	vpp::SubBuffer ubo_;
	vpp::SubBuffer keys0_;
	vpp::SubBuffer keys1_; // temporary buffer we write updates to
	vpp::SubBuffer keysCulled_;

	vpp::SubBuffer indices_;
	vpp::SubBuffer vertices_;

	struct {
		vpp::PipelineLayout pipeLayout;
		vpp::TrDsLayout dsLayout;
		vpp::Pipeline pipe;
		vpp::Pipeline wirePipe;
		vpp::TrDs ds;
	} gfx_;

	struct {
		struct {
			vpp::PipelineLayout pipeLayout;
			vpp::Pipeline pipe;
			vpp::TrDsLayout dsLayout;
			vpp::TrDs ds;
		} update;

		struct {
			vpp::PipelineLayout pipeLayout;
			vpp::Pipeline pipe;
			vpp::TrDsLayout dsLayout;
			vpp::TrDs ds;
		} indirect;

		vpp::SubBuffer dispatch; // indirect dispatch command
	} comp_;

	bool rotateView_ {};
	nytl::Vec2f mposStart_ {};
	nytl::Vec2f mpos_ {};

	tkn::QuatCamera camera_ {};
	bool reload_ {};
	nytl::Mat4f frozenVP_ {};

	bool update_ {false};
	bool updateStep_ {false};

	float rollVel_ {0.f};

	tkn::TimeWidget timeWidget_;
	rvg::Transform windowTransform_;
};

int main(int argc, const char** argv) {
	tkn::appMain<SubdApp>(argc, argv);
}


