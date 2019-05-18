#include <stage/app.hpp>
#include <stage/window.hpp>

class DummyApp : public doi::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!doi::App::init(args)) {
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

	const char* name() const override { return "dummy"; }
};

int main(int argc, const char** argv) {
	DummyApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
