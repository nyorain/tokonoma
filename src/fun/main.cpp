#include <stage/app.hpp>
#include <stage/physics.hpp>
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
#include <Box2D/Collision/Shapes/b2CircleShape.h>
#include <Box2D/Collision/Shapes/b2PolygonShape.h>
#include <Box2D/Collision/Shapes/b2ChainShape.h>

#include <vector>
#include <unordered_set>

class Fun : public doi::App {
public:
	bool init(const doi::AppSettings& settings) override {
		if(!App::init(settings)) {
			return false;
		}

		// rvgContext()._fringe = 0.015;
		physics_.world.SetGravity({0.f, -9.f});

		levelTransform_ = {rvgContext()};
		funPaint_= {rvgContext(), rvg::colorPaint(rvg::Color::white)};

		auto dm = rvg::DrawMode {false, 0.05f};
		dm.aaStroke = true;
		funShape_ = {rvgContext(), {}, dm};

		fun([&](auto x) {
			return nytl::Vec2f {x, /*std::abs(*/x*std::sin(x) + 2 /*)*/};
		});

		player_.paint = {rvgContext(), rvg::colorPaint(rvg::Color::blue)};

		b2CircleShape shape;
		shape.m_radius = 0.2f;
		shape.m_p = {0.f, 0.f};
		player_.rigid = {bworld(), {2.f, 5.f}, shape};
		player_.shape = {rvgContext(), {2.f, 5.f}, 0.2f, {true, 0.f}};

		return true;
	}

	b2World& bworld() {
		return physics_.world;
	}

	template<typename F>
	void fun(F&& f) {
		auto x = -10.f;
		std::vector<nytl::Vec2f> points;
		while(x < 50.f) {
			auto p = f(x);
			points.push_back(p);
			x += 0.1;
		}

		b2ChainShape shape;
		auto bp = reinterpret_cast<const b2Vec2*>(points.data());
		shape.CreateChain(bp, points.size());
		funRigid_ = {bworld(), {0.f, 0.f}, shape, false};

		funShape_.change()->points = std::move(points);
	}

	void update(double dt) override {
		App::update(dt);
		physics_.update(dt);

		auto& kc = *appContext().keyboardContext();
		if(kc.pressed(ny::Keycode::a)) {
			player_.rigid.body->ApplyForceToCenter({-2.5f, 0.}, true);
			player_.rigid.body->ApplyTorque(5.f, true);
		}
		if(kc.pressed(ny::Keycode::d)) {
			player_.rigid.body->ApplyForceToCenter({2.5f, 0.}, true);
			player_.rigid.body->ApplyTorque(-5.f, true);
		}

		auto pos = player_.rigid.body->GetPosition();
		player_.shape.change()->center = {pos.x, pos.y};
	}

	void render(vk::CommandBuffer cb) override {
		auto& ctx = rvgContext();
		ctx.bindDefaults(cb);

		levelTransform_.bind(cb);
		funPaint_.bind(cb);
		funShape_.stroke(cb);

		player_.paint.bind(cb);
		player_.shape.fill(cb);
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);

		auto w = ev.size.x / float(ev.size.y);
		auto h = 1.f;
		auto fac = 40 / std::sqrt(w * w + h * h);

		auto s = nytl::Vec {
			(2.f / (fac * w)),
			(-2.f / (fac * h)), 1
		};

		auto mat = doi::translateMat({0, 2, 0});
		mat = doi::scaleMat({s.x, s.y, 1.f}) * mat;
		mat = doi::translateMat({-1, 1, 0}) * mat;
		levelTransform_.matrix(mat);
	}

protected:
	doi::PhysicsSystem physics_;

	rvg::Shape funShape_;
	rvg::Paint funPaint_;
	rvg::Transform levelTransform_;
	doi::parts::Rigid funRigid_;

	struct {
		doi::parts::Rigid rigid;
		rvg::CircleShape shape;
		rvg::Paint paint;
	} player_;
};

int main(int argc, const char** argv) {
	Fun app;
	if(!app.init({"fun", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}
