#include <stage/app.hpp>
#include <cstdlib>

class ViewApp : public doi::App {
};

int main(int argc, const char** argv) {
	ViewApp app;
	if(!app.init({"3D View", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
