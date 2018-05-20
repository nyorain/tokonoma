#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>

#include <rvg/shapes.hpp>
#include <rvg/context.hpp>
#include <rvg/paint.hpp>

#include <vui/dat.hpp>
#include <vui/gui.hpp>

#include <ny/appContext.hpp>
#include <ny/event.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/key.hpp>

#include <nytl/vec.hpp>
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

		// pendulum
		pendulum_ = {rvgContext(), {400, 250}};
		whitePaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::white)};
		redPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::red)};

		// gui
		using namespace vui::dat;
		auto as = vui::autoSize;
		panel_ = &gui().create<Panel>(nytl::Rect2f {20.f, 0.f, as, as});

		auto createValueTextfield = [&](auto& at, auto name, float& value) {
			auto start = std::to_string(value);
			auto& t = at.template create<Textfield>(name, start).textfield();
			t.onSubmit = [&, name](auto& tf) {
				try {
					value = std::stof(tf.utf8());
				} catch(const std::exception& err) {
					dlg_error("Invalid float for {}: {}", name, tf.utf8());
					return;
				}
			};
		};

		panel_->create<Button>("reset").onClick = [&]{
			pendulum_.angle = 0.f;
			pendulum_.avel = 0.f;
		};

		panel_->create<Button>("slow").onClick = [&]{
			pendulum_.avel = 0.f;
		};

		panel_->create<Button>("push").onClick = [&]{
			pendulum_.avel *= 2.f;
		};

		auto& folder = panel_->create<Folder>("constants");
		createValueTextfield(folder, "mass", pendulum_.constants.m);
		createValueTextfield(folder, "gravity", pendulum_.constants.g);
		createValueTextfield(folder, "length", pendulum_.constants.l);
		createValueTextfield(folder, "j (pen)", pendulum_.constants.j);
		createValueTextfield(folder, "c (pend)", pendulum_.constants.c);

		panel_->toggle();
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

		gui().draw(cb);
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
		// float u = xVel_ * std::pow(xFriction_, dt) - xVel_;
		float u = -xFriction_ * xVel_;
		auto c = pendulum_.center;
		if(c.x < 0.f) {
			c.x = 0.f;
			u = -xVel_ / dt;
		} else if(kc.pressed(ny::Keycode::left)) {
			u -= 5.f;
		}

		if(c.x > window().size().x) {
			c.x = window().size().x;
			u = -xVel_ / dt;
		} else if(kc.pressed(ny::Keycode::right)) {
			u += 5.f;
		}

		xVel_ += dt * u;

		// auto scale = 1.4f * pendulum_.screenLength / pendulum_.constants.l;
		auto scale = 800.f;
		c.x += scale * dt * xVel_;
		pendulum_.changeCenter(c);

		pendulum_.update(dt, u);
	}

protected:
	Pendulum pendulum_;
	rvg::Paint whitePaint_;
	rvg::Paint redPaint_;

	vui::dat::Panel* panel_ {};

	float xVel_ {};
	float xFriction_ {8.f}; // not as it should be...
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
