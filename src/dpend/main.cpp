#include <tkn/app.hpp>
#include <tkn/window.hpp>

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

// TODO: should scale masses and lengths, otherwise it looks wrong.
// might also be wrong though...

// Pendulum
class Pendulum {
public:
	struct {
		// float j {0.05};
		float c1 {0.05};
		float c2 {0.05};

		float l1 {1.};
		float l2 {1.};
		float m1 {2.0};
		float m2 {2.0};
		float g {9.81};
	} constants;


	float angle1 {0.f};
	float angle2 {0.f};
	float vel1 {0.f};
	float vel2 {0.f};
	float accel1 {0.f};
	float accel2 {0.f};

	float screenLength {100.f};

	nytl::Vec2f center;
	rvg::CircleShape fixture;
	rvg::CircleShape mass1;
	rvg::CircleShape mass2;
	rvg::Shape connection1;
	rvg::Shape connection2;

public:
	Pendulum() = default;
	Pendulum(rvg::Context& ctx, nytl::Vec2f pos) : center(pos) {
		constexpr auto centerRadius = 10.f;
		constexpr auto massRadius = 20.f;

		auto m1 = massPos1();
		auto m2 = massPos2();

		auto drawMode = rvg::DrawMode {};
		drawMode.aaFill = true;
		drawMode.fill = true;
		fixture = {ctx, pos, centerRadius, drawMode};
		mass1 = {ctx, m1, massRadius, drawMode};
		mass2 = {ctx, m2, massRadius, drawMode};

		drawMode.fill = false;
		drawMode.stroke = 3.f;
		drawMode.aaStroke = true;
		connection1 = {ctx, {pos, m1}, drawMode};
		connection2 = {ctx, {m1, m2}, drawMode};
	}

	void update(float dt /*, float u = 0.f*/) {
		using std::cos, std::sin, std::pow;

		// logic
		auto& c = constants;
		auto c12 = cos(angle1 - angle2);
		auto s12 = sin(angle1 - angle2);

		// https://de.wikipedia.org/wiki/Doppelpendel
		// seems wrong though
		// auto a1 = -(c.m2 / (c.m1 + c.m2)) * (c.l2 / c.l1) *
		// 	(accel2 * c12 + pow(vel2, 2) * s12) - (c.g / c.l1) * sin(angle1);
		// auto a2 = -(c.l1 / c.l2) * (accel1 * c12 - pow(vel1, 2) * s12) -
		// 	(c.g / c.l2) * sin(angle2);

		// https://www.myphysicslab.com/pendulum/double-pendulum-en.html
		auto a1 = (-c.g * (2 * c.m1 + c.m2) * sin(angle1) - c.m2 * c.g * sin(angle1 - 2*angle2) -
			2 * s12 * c.m2 * (pow(vel2, 2) * c.l2 + pow(vel1, 2) * c.l1 * c12));
		a1 /= (c.l1 * (2 * c.m1 + c.m2 - c.m2 * cos(2 * angle1 - 2 * angle2)));
		// a1 -= c.c1 * vel1;

		auto a2 = 2 * s12 * (pow(vel1, 2) * c.l1 * (c.m1 + c.m2) +
			c.g * (c.m1 + c.m2) * cos(angle1) + pow(vel2, 2) * c.l2 * c.m2 * c12);
		a2 /= (c.l2 * (2 * c.m1 + c.m2 - c.m2 * cos(2 * angle1 - 2 * angle2)));
		// a2 -= c.c2 * vel2;

		angle1 += dt * vel1;
		angle2 += dt * vel2;

		accel1 = a1;
		accel2 = a2;
		vel1 += dt * a1;
		vel2 += dt * a2;

		// rendering
		auto m1 = massPos1();
		auto m2 = massPos2();
		mass1.change()->center = m1;
		mass2.change()->center = m2;
		connection1.change()->points[1] = m1;
		connection2.change()->points[0] = m1;
		connection2.change()->points[1] = m2;
	}

	nytl::Vec2f massPos1() const {
		float c = std::cos(angle1 - 0.5 * nytl::constants::pi);
		float s = std::sin(angle1 - 0.5 * nytl::constants::pi);
		return center + screenLength * nytl::Vec2f{c, s};
	}

	nytl::Vec2f massPos2() const {
		float c = std::cos(angle2 - 0.5 * nytl::constants::pi);
		float s = std::sin(angle2 - 0.5 * nytl::constants::pi);
		return massPos1() + screenLength * nytl::Vec2f{c, s};
	}

	void changeCenter(nytl::Vec2f nc) {
		center = nc;
		auto m1 = massPos1();
		auto m2 = massPos2();
		mass1.change()->center = m1;
		mass2.change()->center = m1;
		connection1.change()->points = {nc, m1};
		connection2.change()->points = {m1, m2};
		fixture.change()->center = nc;
	}
};

// PendulumApp
class PendulumApp : public tkn::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!tkn::App::init(args)) {
			return false;
		}

		// pendulum
		pendulum_ = {rvgContext(), {400, 250}};
		whitePaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::white)};
		redPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::red)};

		// gui
		using namespace vui::dat;
		panel_ = &gui().create<Panel>(nytl::Vec2f{50.f, 0.f}, 300.f, 150.f);

		auto createValueTextfield = [&](auto& at, auto name, float& value) {
			auto start = std::to_string(value);
			start.resize(4);
			auto& t = at.template create<Textfield>(name, start).textfield();
			t.onSubmit = [&, name](auto& tf) {
				try {
					value = std::stof(std::string(tf.utf8()));
				} catch(const std::exception& err) {
					dlg_error("Invalid float for {}: {}", name, tf.utf8());
					return;
				}
			};
		};

		panel_->create<Button>("reset").onClick = [&]{
			pendulum_.angle1 = 0.f;
			pendulum_.angle2 = 0.f;
			pendulum_.vel1 = 0.f;
			pendulum_.vel2 = 0.f;
			pendulum_.accel1 = 0.f;
			pendulum_.accel2 = 0.f;
		};

		auto& fstate = panel_->create<Folder>("state");
		createValueTextfield(fstate, "angle1", pendulum_.angle1);
		createValueTextfield(fstate, "angle2", pendulum_.angle2);

		auto& folder = panel_->create<Folder>("constants");
		createValueTextfield(folder, "mass1", pendulum_.constants.m1);
		createValueTextfield(folder, "mass2", pendulum_.constants.m2);
		createValueTextfield(folder, "gravity", pendulum_.constants.g);
		createValueTextfield(folder, "length1", pendulum_.constants.l1);
		createValueTextfield(folder, "length2", pendulum_.constants.l2);
		// createValueTextfield(folder, "j (pen)", pendulum_.constants.j);
		// createValueTextfield(folder, "c (pend)", pendulum_.constants.c);

		folder.open(false);
		panel_->toggle();

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		rvgContext().bindDefaults(cb);
		windowTransform().bind(cb);

		whitePaint_.bind(cb);
		pendulum_.fixture.fill(cb);
		pendulum_.connection1.stroke(cb);
		pendulum_.connection2.stroke(cb);

		redPaint_.bind(cb);
		pendulum_.mass1.fill(cb);
		pendulum_.mass2.fill(cb);

		gui().draw(cb);
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		auto center = 0.5f * ev.size;
		pendulum_.screenLength = std::min(ev.size.x, ev.size.y) * 0.24 - 30;
		pendulum_.changeCenter(center);
	}

	void update(double dt) override {
		App::update(dt);
		App::scheduleRedraw();

		// input
		// auto& kc = *appContext().keyboardContext();
		// float u = xVel_ * std::pow(xFriction_, dt) - xVel_;
		// float u = -xFriction_ * xVel_;
		// auto c = pendulum_.center;

		/*
		if(manual_) {
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
		} else {
			auto& c = pendulum_.constants;
			auto v = 0.f;

			// works somewhat nicely, after an hour of playing around
			v -= c.m * c.l * c.g * std::sin(pendulum_.angle);
			v += c.c * pendulum_.avel;
			v /= (c.l * c.m * std::cos(pendulum_.angle));
			v *= 3.f;

			// formulas derived from logic.
			// v += std::cos(pendulum_.angle) * pendulum_.avel;
			// v -= std::sin(pendulum_.angle);

			v = std::clamp(v, -10.f, 10.f);
			u += v;
		}

		xVel_ += dt * u;

		// auto scale = 1.4f * pendulum_.screenLength / pendulum_.constants.l;
		auto scale = 400.f;
		c.x += scale * dt * xVel_;
		pendulum_.changeCenter(c);
		*/

		pendulum_.update(dt);

		/*
		// labels
		auto labelState = [&](auto& lbl, auto& val) {
			auto str = std::to_string(val);
			str.resize(std::min<unsigned>(str.size(), 4u));
			lbl.label(str);
		};

		auto a = ((int) nytl::degrees(pendulum_.angle)) % 360;
		labelState(*angleLabel_, a);
		labelState(*xvelLabel_, xVel_);
		labelState(*avelLabel_, pendulum_.avel);
		labelState(*uLabel_, u);
		*/
	}

	// NOTE: test messing with gui
	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		if(ev.pressed && ev.keycode == ny::Keycode::q) {
			auto& f = panel_->create<vui::dat::Folder>("hidden");
			f.create<vui::dat::Button>("Create a kitten");
			f.open(false);
			tmpWidgets_.push_back(&f);
		} else if(ev.pressed && ev.keycode == ny::Keycode::c) {
			if(!tmpWidgets_.empty()) {
				auto w = tmpWidgets_.back();
				tmpWidgets_.pop_back();
				panel_->remove(*w);
			}
		} else {
			return false;
		}

		return true;
	}

	const char* name() const override { return "double pendulum"; }

protected:
	Pendulum pendulum_;
	rvg::Paint whitePaint_;
	rvg::Paint redPaint_;

	vui::dat::Panel* panel_ {};
	vui::dat::Label* angleLabel_ {};
	vui::dat::Label* avelLabel_ {};
	vui::dat::Label* xvelLabel_ {};
	vui::dat::Label* uLabel_ {};

	bool manual_ {};

	std::vector<vui::Widget*> tmpWidgets_ {};

	float xVel_ {};
	float xFriction_ {8.f}; // not as it should be...
};

// main
int main(int argc, const char** argv) {
	PendulumApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}
