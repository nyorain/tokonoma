#include "physics.hpp"
#include <stage/app.hpp>
#include <stage/transform.hpp>
#include <rvg/shapes.hpp>
#include <rvg/paint.hpp>
#include <rvg/context.hpp>
#include <ny/event.hpp>
#include <ny/key.hpp>
#include <ny/appContext.hpp>
#include <ny/keyboardContext.hpp>
#include <dlg/dlg.hpp>

#include <Box2D/Dynamics/b2World.h>
#include <Box2D/Dynamics/Joints/b2MotorJoint.h>
#include <Box2D/Collision/Shapes/b2CircleShape.h>
#include <Box2D/Collision/Shapes/b2PolygonShape.h>

#include <vector>

struct Metal {
	rvg::RectShape shape;
	parts::Rigid rigid;
	rvg::Transform transform;
	b2MotorJoint* motor {};
	bool push {};
};

struct Player {
	rvg::CircleShape shape;
	parts::Rigid rigid;
};

class Mists : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!App::init(settings)) {
			return false;
		}

		levelTransform_ = {rvgContext()};
		metalPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::blue)};
		playerPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::white)};

		// player
		player_.shape = {rvgContext(), {1.f, 1.f}, 0.2f, {true, 0.f}};
		b2CircleShape playerShape;
		playerShape.m_radius = 0.2f;
		playerShape.m_p.SetZero();
		player_.rigid = {bworld(), {1.f, 1.f}, playerShape};
		// player_.rigid.fixture->SetDensity(20.f);

		// metals
		auto createMetal = [&](nytl::Vec2f pos, nytl::Vec2f size, float d) {
			b2PolygonShape shape;
			shape.SetAsBox(size.x / 2, size.y / 2);
			metals_.emplace_back();
			metals_.back().rigid = {bworld(), pos, shape};
			metals_.back().shape = {rvgContext(), -0.5f * size, size,
				{true, 0.f}};
			metals_.back().transform = {rvgContext()};
			metals_.back().rigid.fixture->SetDensity(d);
		};

		createMetal({2, 2}, {0.1, 0.1}, 0.8f);
		createMetal({3, 2}, {0.1, 0.1}, 0.8f);
		createMetal({2, 3}, {0.1, 0.1}, 0.8f);
		createMetal({3, 3}, {0.1, 0.1}, 0.8f);

		// createMetal({2, 2}, {0.5, 0.5}, 1.f);

		// createMetal({3, 4}, {1, 1}, 2.f);
		// createMetal({1, 6}, {0.2, 0.1}, 0.1f);
		// createMetal({5, 0}, {2, 2}, 10.f);

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		rvgContext().bindDefaults(cb);
		metalPaint_.bind(cb);
		for(auto& metal : metals_) {
			metal.transform.bind(cb);
			metal.shape.fill(cb);
		}

		levelTransform_.bind(cb);
		playerPaint_.bind(cb);
		player_.shape.fill(cb);
	}

	void resize(const ny::SizeEvent& ev) override {
		// reset transform
		App::resize(ev);

		auto w = ev.size.x / float(ev.size.y);
		auto h = 1.f;
		auto fac = 10 / std::sqrt(w * w + h * h);

		auto s = nytl::Vec {
			(2.f / (fac * w)),
			(-2.f / (fac * h)), 1
		};

		auto mat = doi::scaleMat({s.x, s.y, 1.f});
		mat = doi::translateMat({-1, 1, 0}) * mat;
		levelTransform_.matrix(mat);
	}

	void update(double dt) override {
		App::update(dt);

		// input
		auto kc = appContext().keyboardContext();
		auto accel = nytl::Vec2f {0.f, 0.f};
		auto fac = 0.5;
		if(kc->pressed(ny::Keycode::w)) {
			accel.y += fac;
		}
		if(kc->pressed(ny::Keycode::a)) {
			accel.x -= fac;
		}
		if(kc->pressed(ny::Keycode::s)) {
			accel.y -= fac;
		}
		if(kc->pressed(ny::Keycode::d)) {
			accel.x += fac;
		}

		player_.rigid.body->ApplyForceToCenter({accel.x, accel.y}, true);

		// physics
		physics_.update(dt);

		for(auto& m : metals_) {
			auto p = m.rigid.body->GetPosition();
			auto r = m.rigid.body->GetAngle();

			auto mat = doi::rotate(r);
			mat = doi::translateMat({p.x, p.y, 0.f}) * mat;
			mat = levelTransform_.matrix() * mat;
			m.transform.matrix(mat);

			if(m.motor) {
				b2Vec2 xB = m.rigid.body->GetPosition();
				auto distance = player_.rigid.body->GetLocalPoint(xB);
				if(m.push) {
					m.motor->SetLinearOffset(100 * distance);
				}

				float force = 0.05 + 0.1 / (distance.LengthSquared() + 1);
				// float force = 0.05 + 0.1 / (distance.Length() + 1);
				m.motor->SetMaxForce(force);
			}
		}

		auto p = player_.rigid.body->GetPosition();
		player_.shape.change()->center = {p.x, p.y};
	}

	void key(const ny::KeyEvent& ev) override {
		std::unordered_map<ny::Keycode, unsigned> assoc = {
			{ny::Keycode::h, 0},
			{ny::Keycode::j, 1},
			{ny::Keycode::k, 2},
			{ny::Keycode::l, 3},
		};

		/*
		if(ev.pressed) {
			if(ev.keycode == ny::Keycode::p) {
				push_ = true;
			} else if(ev.keycode == ny::Keycode::l) {
				push_ = false;
			}
		}
		*/

		if(auto it = assoc.find(ev.keycode); it != assoc.end() && !ev.repeat) {
			if(it->second >= metals_.size()) {
				return;
			}

			auto& metal = metals_[it->second];

			if(metal.motor) {
				dlg_info("Destroyed joint");
				bworld().DestroyJoint(metal.motor);
				metal.motor = nullptr;
			}

			if(!ev.pressed) {
				return;
			}

			b2MotorJointDef def;
			def.Initialize(player_.rigid.body, metal.rigid.body);
			def.maxTorque = 0.f;
			def.correctionFactor = 0.1f;
			def.maxForce = 0.5f;
			def.collideConnected = true;

			bool push = ev.modifiers & ny::KeyboardModifier::ctrl;
			if(push) {
				def.linearOffset *= 100;
			} else {
				def.linearOffset = {};
			}

			dlg_info("Created joint");
			metal.motor = static_cast<b2MotorJoint*>(bworld().CreateJoint(&def));
			metal.push = push;
		}
	}

	b2World& bworld() {
		return physics_.world;
	}


protected:
	PhysicsSystem physics_;
	Player player_;
	std::vector<Metal> metals_;

	rvg::Transform levelTransform_;
	rvg::Paint metalPaint_;
	rvg::Paint playerPaint_;

	// bool push_ {true}; // otherwise pulling
};

int main(int argc, const char** argv) {
	Mists app;
	if(!app.init({"mists", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
