#include <tkn/singlePassApp.hpp>

class DummyApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		((void) cb);
	}

	void update(double dt) override {
		Base::update(dt);
	}

	const char* name() const override { return "dummy"; }
	bool needsDepth() const override { return false; }
};

int main(int argc, const char** argv) {
	return tkn::appMain<DummyApp>(argc, argv);
}
