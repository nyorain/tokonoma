// TODO
// - include atmospheric scattering
//   volumetric light rendering in general
// - maybe make the terrain planet-like and allow to leave atmosphere
// - add tesselation shader
//   evaluate terrain generation in tesselation shader?
//   or use PN triangles?
//   use switch it all to a texture-based terrain generation?
//   that has advantages (performance i guess?) but might introduce
//   a (for good quality potentially huge) memory overhead
//   	- lookup the paper of just using a single triangle/quad for terrain
// - efficient culling

#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/render.hpp>

#include <vpp/trackedDescriptor.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/vk.hpp>

class TerrainApp : public tkn::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		// vk::cmdDraw(cb, size_.x * size_.y, 1, 0, 0);
		vk::cmdDraw(cb, 4, 1, 0, 0); // fullscreen quad
	}

	void update(double dt) override {
		App::update(dt);
	}

	const char* name() const override { return "terrain"; }

public:
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;
	vpp::Pipeline pipe_;
	// vpp::SubBuffer vertices_;
	// vpp::SubBuffer indices_;
	// nytl::Vec2ui size_ {128, 128};
};

int main(int argc, const char** argv) {
	TerrainApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
