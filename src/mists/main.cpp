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
	b2MotorJoint* motor {};
	bool push {};
	rvg::Text label;
};

struct Player {
	rvg::CircleShape shape;
	parts::Rigid rigid;
	b2MotorJoint* moveJoint {};
};

class Mists : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!App::init(settings)) {
			return false;
		}

		// physics_.world.SetWarmStarting(false);

		levelTransform_ = {rvgContext()};
		labelPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::red)};
		playerPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::white)};

		metalPaint_ = rvg::colorPaint({0, 0, 255});
		pushPaint_ = rvg::colorPaint({0, 255, 255});
		pullPaint_ = rvg::colorPaint({255, 0, 255});

		// player
		rvg::DrawMode dm {true, 0.f};
		dm.aaFill = true;
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
			metals_.back().paint = {rvgContext(), metalPaint_};

			auto& font = gui().font();
			metals_.back().label = {rvgContext(), label, font, {}};
		};

		createMetal({2, 2}, {0.05, 0.05}, 2.f, "h");
		createMetal({3, 2}, {0.05, 0.05}, 2.f, "j");
		createMetal({2, 3}, {0.05, 0.05}, 2.f, "k");
		// createMetal({3, 3}, {1.f, 1.f}, 1.f);

		// createMetal({3, 3}, {0.1, 0.1}, 0.8f);
		createMetal({5, 1}, {0.2, 1}, 1.f, "l");
		metals_.back().rigid.body->SetType(b2_staticBody);

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

			if(m.motor) {
				// b2Vec2 bd = m.rigid.body->GetLinearVelocity() -
					// player_.rigid.body->GetLinearVelocity();
				// xB -= 0.5 * player_.rigid.body->GetLinearVelocity();
				// xB += 0.5 * m.rigid.body->GetLinearVelocity();

				// b2Vec2 xB = m.rigid.body->GetPosition();
				// auto distance = player_.rigid.body->GetLocalPoint(xB);

				auto distance = m.rigid.body->GetPosition() - player_.rigid.body->GetPosition();
				auto dn = distance;
				dn.Normalize();

				// distance += m.rigid.body->GetLinearVelocity();
				// distance -= player_.rigid.body->GetLinearVelocity();

				auto fac = 1.f;
				if(m.push) {
					m.motor->SetLinearOffset(distance + dn);
					// m.motor->SetLinearOffset(distance);
				} else {
					m.motor->SetLinearOffset(distance - dn);
					fac = -1.f;
					// m.motor->SetLinearOffset(2 * distance);
				}

				auto d = distance.LengthSquared();

				// inverse square would be like magnetic/gravitational force
				// we use (1 / (dist + 1)) instead of (real) (1 / dist) since
				// this will dampen the falloff. You can still push/pull
				// things somewhat far away

				// float force = 0.5 / (d + 1);
				float force = 0.01 + 0.5 / (std::sqrt(d) + 0.25);
				// float force = 0.01 + 0.1 / (std::pow(d, 0.25) + 0.5);

				auto f1 = -fac * force;
				auto f2 = fac * force * player_.rigid.body->GetMass();

				// alternative force ideas:
				// 1) f1 = m1 * m2 * invDistance; f2 = -f1; -- gravity like
				// 2) f1 = m2 * invDistance; f2 = m1 * invDistance
				//    will make smaller objects even faster; larger ones even
				//    slower. Looks somewhat nicer i think
				// Currently the weight of the player is used as well
				// but this probably conflicts with the original lore

				if(m.rigid.body->GetType() == b2_staticBody) {
					// force *= std::sqrt(player_.rigid.body->GetMass());

					// force *= player_.rigid.body->GetMass();
				} else {
					// force *= 10 * m.rigid.body->GetMass();
					// force *= std::sqrt(player_.rigid.body->GetMass());
					// force *= std::sqrt(m.rigid.body->GetMass());

					// force *= player_.rigid.body->GetMass();
					// force *= m.rigid.body->GetMass();

					f1 *= m.rigid.body->GetMass();
				}

				// m.motor->SetMaxForce(force);
				// m.motor->SetMinForce(force);
				force = std::clamp(force, 0.f, 50.f);
				dlg_debug(force);

				// m.rigid.body->ApplyLinearImpulseToCenter(-fac * force * dn, true);
				// player_.rigid.body->ApplyLinearImpulseToCenter(fac * force * dn, true);
				// m.motor->SetCorrectionFactor(force);

				// m.rigid.body->ApplyForceToCenter(-fac * force * dn, true);
				// player_.rigid.body->ApplyForceToCenter(fac * force * dn, true);

				player_.rigid.body->ApplyForceToCenter(f1 * dn, true);
				m.rigid.body->ApplyForceToCenter(f2 * dn, true);
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

		if(ev.keycode == ny::Keycode::semicolon) {
			push_ = ev.pressed;

			// bad idea for multiple active push/pulls
			/*
			for(auto& active : active_) {
				active->push = push_;
			}
			*/
		}

		if(auto it = assoc.find(ev.keycode); it != assoc.end() && !ev.repeat) {
			if(it->second >= metals_.size()) {
				return;
			}

			auto& metal = metals_[it->second];

			if(metal.motor) {
				dlg_info("Destroyed joint");
				bworld().DestroyJoint(metal.motor);
				metal.motor = nullptr;

				if(!ev.pressed) {
					auto it = active_.find(&metal);
					dlg_assert(it != active_.end());
					active_.erase(it);
				}
			}

			if(!ev.pressed) {
				*metal.paint.change() = metalPaint_;
				return;
			}

			b2MotorJointDef def;
			def.Initialize(player_.rigid.body, metal.rigid.body);
			def.maxTorque = 0.f;
			def.correctionFactor = 0.f;
			def.maxForce = 1.f;
			// def.minForce = 0.1f;
			def.collideConnected = false;

			if(push_) {
				*metal.paint.change() = pushPaint_;
			} else {
				*metal.paint.change() = pullPaint_;
			}

			dlg_info("Created joint");
			metal.motor = static_cast<b2MotorJoint*>(bworld().CreateJoint(&def));
			metal.push = push_;

			active_.insert(&metal);
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
	rvg::Paint playerPaint_;
	rvg::Paint labelPaint_;

	rvg::PaintData metalPaint_;
	rvg::PaintData pushPaint_;
	rvg::PaintData pullPaint_;

	bool push_ {false}; // otherwise pulling
	std::unordered_set<Metal*> active_;
};

int main(int argc, const char** argv) {
	Mists app;
	if(!app.init({"mists", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
