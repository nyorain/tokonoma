#ifdef __cplusplus
	#include <cassert>
	#include <memory>

	#include <tkn/types.hpp>
	#include <tkn/glsl.hpp>
	using namespace tkn::glsl;
	using namespace tkn::types;
	using nytl::constants::pi;

	#define IN(T) const T&
	#define OUT(T) T&

	// sorry, no other sane way.
	// Adding custom constructors to the vec class takes
	// its beautiful aggregrate property away
	#define vec2(x, y) vec2{float(x), float(y)}
	#define vec3(x, y, z) vec3{float(x), float(y), float(z)}
	#define vec4(x, y, z, w) vec4{float(x), float(y), float(z), float(w)}
	#define uvec4(x, y, z, w) uvec4{uint(x), uint(y), uint(z), uint(w)}

	nytl::Vec3f rgb(nytl::Vec4f vec) {
		return {vec[0], vec[1], vec[2]};
	}

	template<std::size_t D>
	struct sampler {
		nytl::Vec<D, unsigned> size;
		std::unique_ptr<nytl::Vec4f[]> data;
	};

	template<std::size_t D>
	nytl::Vec4f texture(const sampler<D>& sampler, nytl::Vec<D, float> coords) {
		// TODO: linear interpolation and such
		// we currently use clampToEdge addressing, nearest neighbor
		auto x = coords * sampler.size;
		x = clamp(x, Vec<D, float>{}, Vec<D, float>(sampler.size) - 1.f);
		auto u = Vec<D, unsigned>(x);
		auto id = 0u;
		for(auto i = 0u; i < D; ++i) {
			auto prod = 1u;
			for(auto j = 0u; j < i; ++j) {
				prod *= sampler.size[j];
			}
			id += prod * u[i];
		}

		return sampler.data[id];
	}

	template<std::size_t D>
	nytl::Vec<D, int> textureSize(const sampler<D>& sampler, int lod) {
		assert(lod == 0u);
		return Vec<D, int>(sampler.size);
	}

	using sampler1D = sampler<1>;
	using sampler2D = sampler<2>;
	using sampler3D = sampler<3>;
#else
	#define IN(T) in T
	#define OUT(T) out T

	// debug hack
	// float one = 1.0;
	// #define assert(x) if(!(x)) { one = 0; }
	#define assert(x)

	const float pi = 3.14159265359;

	vec3 rgb(vec4 v) {
		return v.rgb;
	}
#endif
