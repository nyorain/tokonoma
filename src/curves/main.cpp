#include <stage/app.hpp>
#include <stage/render.hpp>
#include <stage/window.hpp>
#include <stage/geometry.hpp>
#include <nytl/math.hpp>

#include <rvg/shapes.hpp>
#include <rvg/paint.hpp>
#include <rvg/context.hpp>

class CurveApp : public doi::App {
public:
	static constexpr float xoff = 0.1;

public:
	bool init(const doi::AppSettings& settings) override {
		if(!doi::App::init(settings)) {
			return false;
		}

		curvePaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::blue)};
		pointPaint_ = {rvgContext(), rvg::colorPaint(rvg::Color::red)};

		a_ = {rvgContext(), {-1 + xoff, 0.f}, 0.01f, {true, 0.f}, 32};
		b_ = {rvgContext(), {1 - xoff, 0.f}, 0.01f, {true, 0.f}, 32};
		curve_ = {rvgContext(), {}, {false, 0.005f}};
		curve2_ = {rvgContext(), {}, {false, 0.002f}};
		curve3_ = {rvgContext(), {}, {false, 0.002f}};
		curve4_ = {rvgContext(), {}, {false, 0.002f}};

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		rvgContext().bindDefaults(cb);
		pointPaint_.bind(cb);
		a_.fill(cb);
		b_.fill(cb);

		curvePaint_.bind(cb);
		curve_.stroke(cb);
		curve2_.stroke(cb);
		curve3_.stroke(cb);
		curve4_.stroke(cb);
	}

	void update(double delta) override {
		App::update(delta);
		App::scheduleRedraw();

		time_ += delta;
		// amp_ *= std::exp(-0.2 * delta);
		amp_ += delta * ampVel_;
		ampVel_ *= std::exp(-delta); // friction
		ampVel_ += -2.f * delta * amp_; // try to bring amp_ down
		updateCurve();
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		using namespace nytl::vec::cw::operators;
		auto normed = ev.position / nytl::Vec2f(window().size());
		auto pos = 2 * normed - nytl::Vec{1.f, 1.f};
		auto dir = b_.center() - a_.center(); auto normal = doi::rhs::rnormal(dir); // lhs::lnomal
		auto diff = a_.center() - pos;
		a_.change()->center = pos;

		auto dot = nytl::dot(diff, normal);
		ampVel_ -= 0.5 * dot;
		amp_ += 0.4 * dot;
		amp_ = std::clamp(amp_, -1.5f, 1.5f);
	}

	template<typename F>
	std::vector<nytl::Vec2f> discretize(F&& f, float step = 0.01) {
		auto ca = a_.center();
		auto cb = b_.center();
		auto dir = cb - ca;
		auto norm = doi::rhs::rnormal(dir); // rather lhs::lnormal

		auto x = 0.f;
		std::vector<nytl::Vec2f> points;
		while(x < 1.f) {
			auto base = ca + x * dir;
			points.push_back(base + f(x) * norm);
			x += step;
		}

		return points;
	}

	void updateCurve() {
		// f(x): [0, 1] -> [-1, 1]
		using nytl::constants::pi;
		auto dist = nytl::distance(a_.center(), b_.center());
		// float freq = 2 + dist * (5 + std::sin(0.5 * time_));
		// float freq = (4 + amp_ * std::sin(0.5 * time_));
		// float freq = -1.f * std::clamp(amp_, -1.f, 1.f);
		float freq = -amp_ + 0.25 * std::sin(0.5 * time_);
		// float freq = 4 * dist;
		// auto amp = 0.1 + 0.2 * std::cos(time_);
		// auto amp = 0.2;
		// auto amp = amp_;
		// auto amp = 1.f / (1.f + 3 * amp_);
		float amp = 0.2f + 0.05 * std::cos(0.34 * time_);
		auto f = [&](float x) -> float {
			auto y = pi * (freq * x);
			// return amp * (std::sin(y) / y) * std::pow((x - 1) * (x - 1), 0.3);
			// return amp * (std::cos(y) / y - std::sin(y) / (y * y)) * std::pow((x - 1) * (x - 1), 0.3);
			// return amp * (std::cos(y) / y - std::sin(y) / (y * y)) * (x - 1);
			return amp * (std::sin(y)) * (x - 1);
		};

		// NOTE: scale their frequency with the distance, otherwise it
		// makes them look elastic
		auto fi = [&](float offset, float scaleFreq, float scaleAmp) {
			return [=](float x) -> float {
				float freq2 = (12 + 4 * std::sin(offset + scaleFreq * time_)) * dist;
				auto y = pi * (freq2 * x + 1);
				// float a = 0.1 + (0.1 + amp) * (0.5 + 0.1 * std::sin(0.3 * time_));
				float a = 0.03 + 0.01 * std::cos(offset - 0.93 * scaleAmp * time_);
				a *= 5.f;
				return f(x) + a * (std::sin(y) / y) * std::pow(std::abs(x - 1), 0.1);
			};
		};

		curve_.change()->points = discretize(f);
		curve2_.change()->points = discretize(fi(0, 0.19, 0.2), 0.01);
		curve3_.change()->points = discretize(fi(0, -0.19, 0.1), 0.01);
		curve4_.change()->points = discretize(fi(-4.2, -0.32, 0.06), 0.01);
	}

protected:
	rvg::Paint curvePaint_;
	rvg::Paint pointPaint_;
	rvg::Shape curve_;
	rvg::Shape curve2_;
	rvg::Shape curve3_;
	rvg::Shape curve4_;
	rvg::CircleShape a_;
	rvg::CircleShape b_;

	double time_ {};
	float amp_ {};
	float ampVel_ {};
};

int main(int argc, const char** argv) {
	CurveApp app;
	if(!app.init({"curves", {*argv, std::size_t(argc)}})) {
		return EXIT_FAILURE;
	}

	app.run();
}

