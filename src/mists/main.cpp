#include "physics.hpp"
#include <stage/app.hpp>
#include <stage/transform.hpp>
#include <stage/window.hpp>
#include <rvg/shapes.hpp>
#include <rvg/paint.hpp>
#include <rvg/context.hpp>
#include <rvg/text.hpp>
#include <vui/gui.hpp>
#include <ny/event.hpp>
#include <ny/key.hpp>
#include <ny/appContext.hpp>
#include <ny/keyboardContext.hpp>
#include <dlg/dlg.hpp>

#include <Box2D/Dynamics/b2World.h>
#include <Box2D/Dynamics/Contacts/b2Contact.h>
#include <Box2D/Dynamics/b2Fixture.h>
#include <Box2D/Dynamics/Joints/b2MotorJoint.h>
#include <Box2D/Collision/Shapes/b2CircleShape.h>
#include <Box2D/Collision/Shapes/b2PolygonShape.h>

#include <vector>
#include <unordered_set>

struct Metal {
	rvg::RectShape shape;
	rvg::Paint paint;
	parts::Rigid rigid;
	rvg::Transform transform;
	rvg::Text label;

	bool push {};
	bool pull {};
	b2Vec2 oldDir {};
	float goalVel {}; // dooted onto direction
};

struct Player {
	rvg::CircleShape shape;
	parts::Rigid rigid;
	b2MotorJoint* moveJoint {};
};

struct ContactListener : public b2ContactListener {
	void BeginContact(b2Contact* contact);
	void EndContact(b2Contact* contact);

	class Mists* app;
};

class Mists : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!App::init(settings)) {
			return false;
		}

		// physics_.world.SetWarmStarting(false);
		contactListener.app = this;
		physics_.world.SetContactListener(&contactListener);

		levelTransform_ = {rvgContext()};
		labelPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::red)};
		playerPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::white)};

		metalPaint_ = rvg::colorPaint({0, 0, 255});
		pushPaint_ = rvg::colorPaint({0, 255, 255});
		pullPaint_ = rvg::colorPaint({255, 0, 255});

		// player
		rvg::DrawMode dm {true, 0.f};
		dm.aaFill = false;
		player_.shape = {rvgContext(), {1.f, 1.f}, 0.2f, dm};
		b2CircleShape playerShape;
		playerShape.m_radius = 0.2f;
		playerShape.m_p.SetZero();
		player_.rigid = {bworld(), {1.f, 1.f}, playerShape};
		player_.rigid.body->SetLinearDamping(1.4f);
		// player_.rigid.fixture->SetDensity(20.f);

		// metals
		auto createMetal = [&](nytl::Vec2f pos, nytl::Vec2f size, float d,
				std::string label) {
			b2PolygonShape shape;
			shape.SetAsBox(size.x / 2, size.y / 2);
			metals_.emplace_back();
			metals_.back().rigid = {bworld(), pos, shape};
			metals_.back().shape = {rvgContext(), -0.5f * size, size,
				{true, 0.f}};
			metals_.back().transform = {rvgContext()};
			metals_.back().rigid.fixture->SetDensity(d);
			metals_.back().rigid.body->ResetMassData();
			metals_.back().paint = {rvgContext(), metalPaint_};

			auto& font = gui().font();
			metals_.back().label = {rvgContext(), label, font, {}};
		};

		createMetal({2, 2}, {0.05, 0.05}, 0.5f, "h");
		createMetal({3, 1}, {0.05, 0.05}, 1.f, "j");
		createMetal({2, 3}, {0.05, 0.05}, 2.f, "k");
		// createMetal({3, 3}, {1.f, 1.f}, 1.f);

		// createMetal({3, 3}, {0.1, 0.1}, 0.8f);
		createMetal({5, 1}, {0.2, 1}, 1.f, "l");
		metals_.back().rigid.body->SetType(b2_staticBody);

		createMetal({4, 3}, {0.05, 0.05}, 10.f, "i");
		createMetal({5, 3}, {0.05, 0.05}, 100.f, "o");
		// createMetal({2, 2}, {0.5, 0.5}, 1.f);

		// createMetal({3, 4}, {1, 1}, 2.f);
		// createMetal({1, 6}, {0.2, 0.1}, 0.1f);
		// createMetal({5, 0}, {2, 2}, 10.f);

		// NOTE: test
		// b2MotorJointDef mjd;
		// mjd.Initialize(player_.rigid.body, metals_.back().rigid.body);
		// mjd.maxForce = 0.5f;
		// mjd.maxTorque = 0.f;
		// mjd.correctionFactor = 0.5f;
		// mjd.collideConnected = true;
		// player_.moveJoint = (b2MotorJoint*) bworld().CreateJoint(&mjd);

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		rvgContext().bindDefaults(cb);
		for(auto& metal : metals_) {
			metal.paint.bind(cb);
			metal.transform.bind(cb);
			metal.shape.fill(cb);

			labelPaint_.bind(cb);
			windowTransform().bind(cb);
			metal.label.draw(cb);
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
		App::redraw();

		// input
		auto kc = appContext().keyboardContext();
		auto accel = nytl::Vec2f {0.f, 0.f};
		auto fac = 0.01;
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

		// player_.rigid.body->ApplyForceToCenter({accel.x, accel.y}, true);
		player_.rigid.body->ApplyLinearImpulseToCenter({accel.x, accel.y}, true);
		// auto vel = player_.rigid.body->GetLinearVelocity();
		// vel += b2Vec2 {accel.x, accel.y};
		// player_.rigid.body->SetLinearVelocity(vel);
		// b2Vec2 pp = metals_.back().rigid.body->GetPosition();
		// auto distance = player_.rigid.body->GetLocalPoint(pp);
		// distance.x += accel.x;
		// distance.y += accel.y;
		// player_.moveJoint->SetLinearOffset(distance);

		// physics
		physics_.update(dt);

		for(auto& m : metals_) {
			auto p = m.rigid.body->GetPosition();
			auto r = m.rigid.body->GetAngle();

			auto mat = doi::rotateMat(r);
			mat = doi::translateMat({p.x, p.y, 0.f}) * mat;
			mat = levelTransform_.matrix() * mat;
			m.transform.matrix(mat);

			nytl::Vec2f pp {p.x, p.y};
			auto t = levelTransform_.matrix() * nytl::Vec4f{pp.x, pp.y, 0.f, 1.f};
			pp = {t[0], t[1]};
			pp.x = 0.5f * pp.x + 0.5f;
			pp.y = 0.5f * pp.y + 0.5f;
			pp.x *= window().size().x;
			pp.y *= window().size().y;
			pp.x -= 20;
			m.label.change()->position = pp;

			if(m.push || m.pull) {
				auto distance = m.rigid.body->GetPosition() - player_.rigid.body->GetPosition();
				auto dn = distance;
				auto d = dn.Normalize();

				auto fac = m.push ? 1.f : -1.f;
				dn *= fac;

				// force-distance falloff
				float impulse = 0.5 / (d + 0.25);
				// float impulse = std::exp(-0.1 * d);

				// mass factors
				impulse *= player_.rigid.body->GetMass();
				if(m.rigid.body->GetType() == b2_dynamicBody) {
					impulse *= 1 - (std::exp(-10 * m.rigid.body->GetMass()));
				}

				// correction (collisions, other allomancer from other dir)
				// if metal came out too slow we increase force
				// allows flying when puhsing against coin on ground
				auto dv = b2Dot(m.rigid.body->GetLinearVelocity(), m.oldDir);
				if (m.goalVel - dv > 0) {
					// NOTE: rather use that to calculate a pseudo "mass"
					// holding against our push from the other side
					// impulse += m.rigid.body->GetMass() * (m.goalVel - dv);
					impulse += 0.05 * (m.goalVel - dv);
				}

				impulse = std::clamp(impulse, 0.f, 0.05f);
				// dlg_debug("impulse: {}", impulse);

				auto i1 = -impulse;
				auto i2 = impulse;

				// alternative force ideas:
				// 1) f1 = m1 * m2 * invDistance; f2 = -f1; -- gravity like
				// 2) f1 = m2 * invDistance; f2 = m1 * invDistance
				//    will make smaller objects even faster; larger ones even
				//    slower. Looks somewhat nicer i think
				// Currently the weight of the player is used as well
				// but this probably conflicts with the original lore

				i2 *= player_.rigid.body->GetMass();

				// m.rigid.body->ApplyLinearImpulseToCenter(-fac * force * dn, true);
				// player_.rigid.body->ApplyLinearImpulseToCenter(fac * force * dn, true);
				// m.motor->SetCorrectionFactor(force);

				// m.rigid.body->ApplyForceToCenter(-fac * force * dn, true);
				// player_.rigid.body->ApplyForceToCenter(fac * force * dn, true);

				player_.rigid.body->ApplyLinearImpulseToCenter(i1 * dn, true);
				m.rigid.body->ApplyLinearImpulseToCenter(i2 * dn, true);

				m.oldDir = dn;
				m.goalVel = b2Dot(dn, m.rigid.body->GetLinearVelocity());
			}
		}

		auto p = player_.rigid.body->GetPosition();
		player_.shape.change()->center = {p.x, p.y};
	}

	bool key(const ny::KeyEvent& ev) override {
		if(App::key(ev)) {
			return true;
		}

		std::unordered_map<ny::Keycode, unsigned> assoc = {
			{ny::Keycode::h, 0},
			{ny::Keycode::j, 1},
			{ny::Keycode::k, 2},
			{ny::Keycode::l, 3},
			{ny::Keycode::i, 4},
			{ny::Keycode::o, 5},
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

		if(ev.keycode == ny::Keycode::space) {
			push_ = ev.pressed;
		}

		if(auto it = assoc.find(ev.keycode); it != assoc.end() && !ev.repeat) {
			if(it->second >= metals_.size()) {
				return false;
			}

			auto& metal = metals_[it->second];

			if(!ev.pressed) {
				*metal.paint.change() = metalPaint_;
				metal.push = metal.pull = false;
				return true;
			}

			if(push_) {
				*metal.paint.change() = pushPaint_;
				metal.push = true;
			} else {
				*metal.paint.change() = pullPaint_;
				metal.pull = true;
			}

			metal.goalVel = 0;
			metal.oldDir.SetZero();
			return true;
		}

		return false;
	}

	b2World& bworld() {
		return physics_.world;
	}


protected:
	PhysicsSystem physics_;
	Player player_;
	std::vector<Metal> metals_;

	rvg::Transform levelTransform_;
	rvg::Paint playerPaint_;
	rvg::Paint labelPaint_;

	rvg::PaintData metalPaint_;
	rvg::PaintData pushPaint_;
	rvg::PaintData pullPaint_;

	bool push_ {false}; // otherwise pulling
	ContactListener contactListener {};

	friend ContactListener;
};

void ContactListener::BeginContact(b2Contact* c) {
	auto p = app->player_.rigid.fixture;
	if(c->GetFixtureA() != p && c->GetFixtureB() != p) {
		return;
	}

	// NOTE: here should happen some magic...
	// the basic damage applied is basically just how large the difference
	// in moementum between the colliding entities is

	// TODO:
	// but in addition to that, we also take the "collision area" into account,
	// ie. what is responsible that extremely small objects hitting us even
	// with medium force can do serious damage (meaning: a small needle
	// with 10kmh makes more damage than a ball with 10kmh, even though
	// the ball has more mass and therefore a larger difference of momentum).
	//
	// not sure if this is something we can actually do, how to get something
	// like the contact size? the contact/manifold and its data depend too
	// much on frame timing to be of any use.
	// idea: move the objects by one/two times their current velocity and
	// see how deep they are then or something like that?

	// TODO:
	// we should probably not do this in BeginContact, but rather apply
	// it every frame is post/pre solve

	auto metal = c->GetFixtureA() == p ? c->GetFixtureB() : c->GetFixtureA();

	 b2WorldManifold worldManifold;
  	c->GetWorldManifold( &worldManifold );

	// TODO: probably add up for all manifold points?
	b2Vec2 vel1 = app->player_.rigid.body->
		GetLinearVelocityFromWorldPoint(worldManifold.points[0]);
  	b2Vec2 vel2 = metal->GetBody()->
		GetLinearVelocityFromWorldPoint(worldManifold.points[0]);

	auto mom1 = app->player_.rigid.body->GetMass() * vel1;
	auto mom2 = metal->GetBody()->GetMass() * vel2;
  	b2Vec2 impactVelocity = mom1 - mom2;
	dlg_debug("damage: {}", impactVelocity.Length());
}

void ContactListener::EndContact(b2Contact*) {
	// no op
}

int main(int argc, const char** argv) {
	Mists app;
	if(!app.init({"mists", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
