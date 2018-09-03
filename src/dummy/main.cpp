#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>

class DummyApp : public doi::App {
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
	DummyApp app;
	if(!app.init({"dummy", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
