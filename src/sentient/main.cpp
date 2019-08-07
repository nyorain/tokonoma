#include <tkn/app.hpp>
#include <tkn/window.hpp>
#include <tkn/bits.hpp>

#include <nytl/vec.hpp>
#include <nytl/math.hpp>
#include <nytl/mat.hpp>
#include <nytl/matOps.hpp>

#include <vpp/sharedBuffer.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>

#include <shaders/sentient.sentient.frag.h>
#include <shaders/sentient.sentient.vert.h>

// idea: 2D dynamically animated creatures
// highly WIP, nothing viable yet

class CreaturesApp : public tkn::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
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

	const char* name() const override { return "creatures"; }
};

int main(int argc, const char** argv) {
	CreaturesApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
