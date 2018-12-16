#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/transform.hpp>
#include <stage/window.hpp>

#include <vpp/fwd.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>

#include <nytl/mat.hpp>

#include <optional>

class HexApp : public doi::App {
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

protected:
	vpp::SubBuffer buffer_;

	vpp::TrDsLayout compDsLayout_;
	vpp::PipelineLayout compPipelineLayout_;
	vpp::Pipeline compPipeline_;

	// render
	vpp::SubBuffer gfxUbo_;
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

int main(int argc, const char** argv) {
	HexApp app;
	if(!app.init({"hex", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
