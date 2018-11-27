// NOTE:
// tanh: Maps any float number on the range (-1, 1)
const float pi = 3.1415926535897932;
const float sqrtpi = 1.7724538509055160;

float sinc(float x) {
	return sin(x) / x;
}

// https://en.wikipedia.org/wiki/Hyperbolic_function
float sech(float x) { return 1.f / cosh(x); } // hyperbolic secant
float csch(float x) { return 1.f / sinh(x); } // hyperbolic cosecant
float coth(float x) { return cosh(x) / sinh(x); } // hyperbolic cotan

// like sin,cos but with more arc-like; rounded
// also have smaller amplitude (~0.76)
float roundsin(float x) { return tanh(sin(x)); }
float roundcos(float x) { return tanh(cos(x)); }

// Like smoothstep but smoother
float smootherstep(float edge0, float edge1, float x) {
  x = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

// looks roughly like a heartbeat (on a monitor) around the origin
// derivate of tanh(x * x)
float heartbeat(float x) {
	float s = sech(x * x);
	return 2 * x * s * s;
}

// see https://en.wikipedia.org/wiki/Dirac_delta_function
// Approximation of the dirac delta "function" for a -> 0.
// Only well defined for a > 0.
// Basically like exp(-x^2) around 0, but always has the full integral 1
// and gets steeper (maximum at 0 -> inf) for a -> 0.
float dirac(float a, float x) {
	x /= a;
	return 1 / (a * sqrtpi) * exp(-x * x);
}

// (-inf, inf) -> (low, high)
// Interpolates between low and high using tanh.
// interp(low, high, 0) := 0.5 * (low + high)
// interp(low, high, -> inf) := high
// interp(low, high, -> -inf) := low
float interp(float low, float high, float x) {
	return low + 0.5 * (high - low) * tanh(x);
}

// NOTE: all following ripped from
// http://iquilezles.org/www/articles/functions/functions.htm
// from 0: growing fast, then decaying slowly
// maximum at 1/k, so basically k says how steep the initial growth is
float impulse(float k, float x) {
	const float y = k * x;
	return y * exp(1 - y);
}

// Like a symmetrical smoothstep with 0 on both ends.
// Uses exactly the same interpolation function
// as std smoothstep. Expects width > 0
// The same as: smoothstep(c - w, c, x) - smoothstep(c, c + w, x);
float smoothcurve(float center, float width, float x) {
	x = min(1, abs((x - center) / width));
	return 1.0 - x * x * (3.0 - 2.0 * x);
}

// [0, 1] to [0, 1]: 0, 1 -> 0 and 0.5 -> 1
// The larger k, the steeper. Penis-like for k -> 0
// Expects k > 0.
float parabola(float x, float k) {
	return pow(4 * x * (1 - x), k);
}

// Like pcurve (see below) but scaled: doesn't have a maximum at 1.
float pcurveScaled(float x, float a, float b) {
	return pow(x, a) * pow(1.0 - x, b);
}

// [0, 1] to [0, 1]: 0, 1 -> 0 and somewhere in between -> 1
// a: How steep it grows left from the hill.
// b: How steep it grows right from the hill.
// When a and b are equal, the maximum will be at 0.5.
// When e.g. a is larger, it will be near 1.
// Called powercurve by iq.
float pcurve(float x, float a, float b) {
	float k = pow(a + b, a + b) / (pow(a, a) * pow(b, b));
	return k * pcurveScaled(x, a, b);
}

// TODO: gain; some more others
// smootherstep (not to iq functions)
