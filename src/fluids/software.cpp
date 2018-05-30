#include <ny/backend.hpp>
#include <ny/appContext.hpp>
#include <ny/windowContext.hpp>
#include <ny/bufferSurface.hpp>
#include <ny/windowSettings.hpp>
#include <nytl/vec.hpp>
#include <nytl/vecOps.hpp>
#include <dlg/dlg.hpp>
#include <chrono>

class WindowListener : public ny::WindowListener {
public:
	bool* run {};
	nytl::Vec2ui size {};
	nytl::Vec2i mpos {};
	void resize(const ny::SizeEvent& ev) { size = ev.size; }
	void close(const ny::CloseEvent&) { *run = false; }
	void mouseMove(const ny::MouseMoveEvent& ev) { mpos = ev.position; }
};

template<typename T>
class Field {
public:
	void resize(nytl::Vec2ui);

	T& operator()(nytl::Vec2ui);
	T& operator()(unsigned x, unsigned y) { return (*this)({x, y}); }
	const T& operator()(nytl::Vec2ui) const;
	const T& operator()(unsigned x, unsigned y) const { return (*this)({x, y}); }
	const T sample(nytl::Vec2f) const;
	const auto& size() const { return size_; }

protected:
	nytl::Vec2ui size_;
	std::vector<T> vals_;
};

class FluidSystem {
public:
	FluidSystem(nytl::Vec2ui size);

	void update(float dt);

	void resize(nytl::Vec2ui);
	nytl::Vec2f velocity(nytl::Vec2ui);
	nytl::Vec2f velocitys(nytl::Vec2f); // sampled
	float density(nytl::Vec2ui);
	float densitys(nytl::Vec2f); // sampled

	const auto& size() const { return size_; }
	const auto& density() const { return *density_; }

	auto& velocity() { return *vel_; }
	auto& velocity0() { return *vel0_; }

protected:
	void velocityStep(float dt);
	void densityStep(float dt);
	void projectVelocity();

protected:
	nytl::Vec2ui size_;

	Field<nytl::Vec2f> vels_ [2];
	Field<float> densitys_ [2];

	Field<nytl::Vec2f>* vel_;
	Field<nytl::Vec2f>* vel0_;

	Field<float> p_;
	Field<float> div_;

	Field<float>* density_;
	Field<float>* density0_;
};

int main() {
	// window stuff
	auto& backend = ny::Backend::choose();
	auto ac = backend.createAppContext();

	auto listener = WindowListener {};
	ny::BufferSurface* bufferSurface {};
	auto ws = ny::WindowSettings {};

	ws.listener = &listener;
	ws.surface = ny::SurfaceType::buffer;
	ws.buffer.storeSurface = &bufferSurface;
	auto wc = ac->createWindowContext(ws);

	if(!ac->pollEvents()) {
		dlg_info("pollEvents returned false");
		return EXIT_FAILURE;
	}

	// system
	auto size = nytl::Vec2ui {800, 500};
	FluidSystem system(size);

	// run
	auto run = true;
	listener.run = &run;
	listener.size = size;

	using Clock = std::chrono::high_resolution_clock;
	using Secf = std::chrono::duration<float, std::ratio<1, 1>>;
	auto lastFrame = Clock::now();
	auto mpos = listener.mpos;

	while(run) {
		if(!ac->pollEvents()) {
			dlg_info("pollEvents returned false");
			break;
		}

		// resized?
		if(size != listener.size) {
			dlg_info("resized");
			size = listener.size;
			system.resize(size);
		}

		// update
		auto now = Clock::now();
		auto diff = now - lastFrame;
		auto dt = std::chrono::duration_cast<Secf>(diff).count();
		lastFrame = now;
		dlg_info("frametime: {}", dt);
		system.update(dt);

		auto mdiff = listener.mpos - mpos;
		mpos = listener.mpos;
		system.velocity()(nytl::Vec2ui(mpos)) += 5 * dt * mdiff;
		system.velocity0()(nytl::Vec2ui(mpos)) += 5 * dt * mdiff;

		// render
		{
			auto guard = bufferSurface->buffer();
			auto img = guard.get();
			dlg_assert(img.size == size);

			for(auto y = 0u; y < size.y; ++y) {
				for(auto x = 0u; x < size.x; ++x) {
					auto d = system.density(nytl::Vec2ui {x, y});
					auto c = uint8_t(std::min(d * 255, 255.f));
					ny::writePixel(img, {x, y}, {c, c, c, 255});
				}
			}
		}
	}
}

// util
template<typename C>
auto sample(const C& c, nytl::Vec2f pos, nytl::Vec2ui size) {
	using namespace nytl::vec::cw;
	auto [x, y] = clamp(pos, nytl::Vec {1.f, 1.f}, size - nytl::Vec{1.f, 1.f});

	// -+--------+--------+-
	//  |        |        |
	//	| x0, y0 | x1, y0 |
	//  |        |        |
	// -+------(x,y)------+-
	//  |        |        |
  	//	| x0, y1 | x1, y1 |
	//  |        |        |
	// -+--------+--------+-

	auto x0 = (unsigned) x;
	auto y0 = (unsigned) y;
	auto x1 = x0 + 1;
	auto y1 = y0 + 1;

	auto s1 = x - x0; // x1 fac
	auto s0 = 1.f - s1; // x0 fac
	auto t1 = y - y0; // y1 fac
	auto t0 = 1.f - t1; // y0 fac

	auto v = [&](auto x, auto y) {
		return c[y * size.x + x];
	};

	return (s0 * (t0 * v(x0, y0) + t1 * v(x0, y1)) +
		s1 * (t0 * v(x1, y0) + t1 * v(x1, y1)));
}

template<typename T>
void Field<T>::resize(nytl::Vec2ui size) {
	// TODO: this will fuck everything up...
	// we should to it row based but that is more complicated
	size_ = size;
	vals_.resize(size.x * size.y);
}

template<typename T>
T& Field<T>::operator()(nytl::Vec2ui pos) {
	return vals_[pos.y * size_.x + pos.x];
}

template<typename T>
const T& Field<T>::operator()(nytl::Vec2ui pos) const {
	return vals_[pos.y * size_.x + pos.x];
}

template<typename T>
const T Field<T>::sample(nytl::Vec2f pos) const {
	return ::sample(vals_, pos, size_);
}

template<typename T>
void setBoundary(Field<T>& f, int b) {
	auto ex = f.size().x - 1;
	for(auto y = 1u; y < f.size().y - 2; ++y) {
		f(0, y) = b == 1 ? -f(1, y) : f(1, y);
		f(ex, y) = b == 1 ? -f(ex - 1, y) : f(ex - 1, y);
	}

	auto ey = f.size().y - 1;
	for(auto x = 1u; x < f.size().x - 2; ++x) {
		f(x, 0) = b == 2 ? -f(x, 1) : f(x, 1);
		f(x, ey) = b == 2 ? -f(x, ey - 1) : f(x, ey - 1);
	}

	f(0, 0) = 0.5f * (f(1, 0) + f(0, 1));
	f(0, ey) = 0.5f * (f(1, ey) + f(0, ey - 1));
	f(ex, 0) = 0.5f * (f(ex - 1, 0) + f(ex, 1));
	f(ex, ey) = 0.5f * (f(ex - 1, ey) + f(ex, ey - 1));
}

template<typename T>
void diffuse(Field<T>& f, const Field<T>& f0, float diff, float dt,
		int boundary) {

	constexpr auto iters = 5;
	auto a = dt * diff * f.size().x * f.size().y;

	for(auto k = 0u; k < iters; ++k) {
		for(auto x = 1u; x < f.size().x - 2; ++x) {
			for(auto y = 1u; y < f.size().y - 2; ++y) {
				auto sum = f(x - 1, y) + f(x + 1, y) + f(x, y - 1) + f(x, y + 1);
				f(x, y) = (1.f / (1 + 4 * a)) * (f0(x, y) + a * sum);
			}
		}

		setBoundary(f, boundary);
	}
}

template<typename T>
void advect(Field<T>& f, const Field<T>& f0, const Field<nytl::Vec2f>& vel,
		float dt, int boundary) {

	dt *= f.size().x;
	for(auto x = 1u; x < f.size().x - 2; ++x) {
		for(auto y = 1u; y < f.size().y - 2; ++y) {
			auto bt = nytl::Vec2ui {x, y} - dt * vel(x, y);
			f(x, y) = f0.sample(bt);
		}
	}

	setBoundary(f, boundary);
}

// FluidSystem
FluidSystem::FluidSystem(nytl::Vec2ui size) : size_(size) {
	vels_[0].resize(size_);
	vels_[1].resize(size_);
	densitys_[0].resize(size_);
	densitys_[1].resize(size_);

	vel_ = &vels_[0];
	vel0_ = &vels_[1];

	density_ = &densitys_[0];
	density0_ = &densitys_[1];

	p_.resize(size);
	div_.resize(size);
}

void FluidSystem::update(float dt) {
	velocityStep(dt);
	densityStep(dt);
}

void FluidSystem::resize(nytl::Vec2ui size) {
	size_ = size;
	vels_[0].resize(size_);
	vels_[1].resize(size_);
	densitys_[0].resize(size_);
	densitys_[1].resize(size_);
	p_.resize(size);
	div_.resize(size);
}

nytl::Vec2f FluidSystem::velocity(nytl::Vec2ui pos) {
	return (*vel_)(pos);
}

float FluidSystem::density(nytl::Vec2ui pos) {
	return (*density_)(pos);
}

void FluidSystem::velocityStep(float dt) {
	// add a source in the center
	/*
	auto center = 0.5 * size_;
	constexpr auto radius = 100;
	for(auto x = -radius; x <= radius; ++x) {
		for(auto y = -radius; y <= radius; ++y) {
			auto pos = nytl::Vec2ui(center + nytl::Vec {x, y});
			auto vec = 0.05 * dt * nytl::Vec {std::cos(x / 300.f), -0.5};
			(*vel_)(pos) += vec;
			(*vel0_)(pos) += vec;
		}
	}
	*/

	// std::swap(vel_, vel0_);
	diffuse(*vel_, *vel0_, 0.1f, dt, 1);
	projectVelocity();
	std::swap(vel_, vel0_);
	advect(*vel_, *vel0_, *vel0_, dt, 1); // TODO: wrong boundary
	projectVelocity();

	// *vel0_ = *vel_;
	// std::swap(vel_, vel0_);
}

void FluidSystem::densityStep(float dt) {
	// add a source in the center
	auto center = 0.5 * size_;
	constexpr auto radius = 100;
	for(auto x = -radius; x <= radius; ++x) {
		for(auto y = -radius; y <= radius; ++y) {
			auto pos = nytl::Vec2ui(center + nytl::Vec {x, y});
			(*density0_)(pos) += dt * 0.02f;
			(*density_)(pos) += dt * 0.02f;
		}
	}

	// std::swap(density_, density0_);
	diffuse(*density_, *density0_, 1.f, dt, 0);
	std::swap(density_, density0_);
	advect(*density_, *density0_, *vel_, dt, 0);

	// *density0_ = *density_;
	// std::swap(density_, density0_);
}

void FluidSystem::projectVelocity() {
	unsigned int i, j, k;
	float h;
	auto N = size_.x - 2;
	h = 1.0 / N;

	auto& v = *vel_;

	for ( i=1 ; i <= size_.x - 2; i++ ) {
		for ( j=1 ; j<= size_.y - 2 ; j++ ) {
			auto u0 = v(i + 1, j).x;
			auto u1 = v(i - 1, j).x;
			auto v0 = v(i, j + 1).y;
			auto v1 = v(i, j - 1).y;

			div_(i,j) = -0.5 * h * (u0 - u1 + v0 - v1);
			p_(i,j) = 0.f;
		}
	}

	setBoundary(div_, 0);
	setBoundary(p_, 0);

	for ( k=0 ; k<5 ; k++ ) {
		for ( i=1 ; i<=size_.x - 2 ; i++ ) {
			for ( j=1 ; j<=size_.y - 2 ; j++ ) {
				p_(i,j) = (div_(i,j)+p_(i-1,j)+p_(i+1,j)+ p_(i,j-1)+p_(i,j+1))/4;
			}
		}

		setBoundary(p_, 0);
	}

	for ( i=1 ; i<= size_.x - 2; i++ ) {
		for ( j=1 ; j<= size_.y - 2; j++ ) {
			auto u0 = p_(i + 1, j);
			auto u1 = p_(i - 1, j);
			auto v0 = p_(i, j + 1);
			auto v1 = p_(i, j - 1);

			v(i,j).x -= 0.5*(u0 - u1)/h;
			v(i,j).y -= 0.5*(v0 - v1)/h;
		}
	}

	// setBoundary( N, 1, u );
	setBoundary(v, 1);
}
