// WIP

#include <tkn/app.hpp>
#include <tkn/window.hpp>

class TerrainApp : public tkn::App {
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

	void update(double dt) override {
		App::update(dt);
	}

	const char* name() const override { return "terrain"; }
};

int main(int argc, const char** argv) {
	TerrainApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
