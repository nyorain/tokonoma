#include <tkn/util.hpp>
#include <cmath>

namespace tkn {

nytl::Vec3f blackbody(unsigned temp) {
	// http://www.zombieprototypes.com/?p=210
	// this version based upon https://github.com/neilbartlett/color-temperature,
	// licensed under MIT.
	float t = temp / 100.f;
	float r, g, b;

	struct Coeffs {
		float a, b, c, off;
		float compute(float x) {
			auto r = a + b * x + c * std::log(x + off);
			return std::clamp(r, 0.f, 255.f);
		}
	};

	if(t < 66.0) {
		r = 255;
	} else {
		r = Coeffs {
			351.97690566805693,
			0.114206453784165,
			-40.25366309332127,
			-55
		}.compute(t);
	}

	if(t < 66.0) {
		g = Coeffs {
			-155.25485562709179,
			-0.44596950469579133,
			104.49216199393888,
			-2
		}.compute(t);
	} else {
		g = Coeffs {
			325.4494125711974,
			0.07943456536662342,
			-28.0852963507957,
			-50
		}.compute(t);
	}

	if(t >= 66.0) {
		b = 255;
	} else {

		if(t <= 20.0) {
			b = 0;
		} else {
			b = Coeffs {
				-254.76935184120902,
				0.8274096064007395,
				115.67994401066147,
				-10
			}.compute(t);

		}
	}

	return {r / 255.f, g / 255.f, b / 255.f};
}

} // namespace tkn
