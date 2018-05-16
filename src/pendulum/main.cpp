#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>

#include <rvg/shapes.hpp>
#include <rvg/context.hpp>
#include <rvg/paint.hpp>
#include <nytl/vec.hpp>
#include <ny/appContext.hpp>
#include <ny/event.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/key.hpp>
#include <dlg/dlg.hpp>

// Pendulum
class Pendulum {
public:
	struct {
		float j {0.05};
		float c {0.05};
		float l {0.5};
		float m {0.5};
		float g {9.81};
	} constants;

	float screenLength {200.f};
	float angle {0.01f};
	float avel {0.f};

	nytl::Vec2f center;
	rvg::CircleShape fixture;
	rvg::CircleShape mass;
	rvg::Shape connection;

public:
	Pendulum() = default;
	Pendulum(rvg::Context& ctx, nytl::Vec2f pos) : center(pos) {
		constexpr auto centerRadius = 10.f;
		constexpr auto massRadius = 20.f;

		auto end = massPos();
		auto drawMode = rvg::DrawMode {};
		drawMode.aaFill = true;
		drawMode.fill = true;
		fixture = {ctx, pos, centerRadius, drawMode};
		mass = {ctx, end, massRadius, drawMode};

		drawMode.fill = false;
		drawMode.stroke = 3.f;
		drawMode.aaStroke = true;
		connection = {ctx, {pos, end}, drawMode};
	}

	void update(float dt, float u = 0.f) {
		// logic
		angle += dt * avel;

		auto& c = constants;
		auto accel = c.m * c.l * (u * std::cos(angle) + c.g * std::sin(angle));
		accel -= c.c * avel;
		accel /= (c.j + c.m * c.l * c.l);
		avel += dt * accel;

		// rendering
		auto end = massPos();
		mass.change()->center = end;
		connection.change()->points[1] = end;
	}

	nytl::Vec2f massPos() const {
		float c = std::cos(angle + 0.5 * nytl::constants::pi);
		float s = std::sin(angle + 0.5 * nytl::constants::pi);
		return center + screenLength * nytl::Vec2f{c, s};
	}

	void changeCenter(nytl::Vec2f nc) {
		center = nc;
		auto end = massPos();
		mass.change()->center = end;
		connection.change()->points = {nc, end};
		fixture.change()->center = nc;
	}
};

// PendulumApp
class PendulumApp : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		pendulum_ = {rvgContext(), {400, 250}};
		whitePaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::white)};
		redPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::red)};
		renderer().invalidate();
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		rvgContext().bindDefaults(cb);
		windowTransform().bind(cb);

		whitePaint_.bind(cb);
		pendulum_.fixture.fill(cb);
		pendulum_.connection.stroke(cb);

		redPaint_.bind(cb);
		pendulum_.mass.fill(cb);
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		auto center = 0.5f * ev.size;
		pendulum_.screenLength = std::min(ev.size.x, ev.size.y) * 0.48 - 30;
		pendulum_.changeCenter(center);
	}

	void update(double dt) override {
		App::update(dt);

		// input
		auto& kc = *appContext().keyboardContext();
		float u = -xFriction_ * xVel_;
		auto c = pendulum_.center;
		if(c.x < 0.f) {
			c.x = 0.f;
			u = -xVel_ / dt;
		} else if(kc.pressed(ny::Keycode::left)) {
			u -= 200.f;
		}

		if(c.x > window().size().x) {
			c.x = window().size().x;
			u = -xVel_ / dt;
		} else if(kc.pressed(ny::Keycode::right)) {
			u += 200.f;
		}

		u *= dt;
		xVel_ += u;

		auto scale = 0.02 * pendulum_.screenLength / pendulum_.constants.l;
		c.x += scale * dt * xVel_;
		pendulum_.changeCenter(c);

		pendulum_.update(dt, u);
	}

protected:
	Pendulum pendulum_;
	rvg::Paint whitePaint_;
	rvg::Paint redPaint_;

	float xVel_ {};
	float xFriction_ {0.95};
};

// main
int main(int argc, const char** argv) {
	PendulumApp app;
	if(!app.init({"pendulum", {*argv, std::size_t(argc)}})) {
		dlg_fatal("Initialization failed");
		return EXIT_FAILURE;
	}

	app.run();
}
