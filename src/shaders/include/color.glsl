// See http://iquilezles.org/www/articles/palettes/palettes.htm
// a: color offset (base color) [0.5, 0.5, 0.5]
// b: cos to color; a ± b must be in [0, 1]; [0.5, 0.5, 0.5]
// c: how fast t is moving for the different components [1, 1, 1]
// d: cos offset [0, 0.25, 0.5]
vec3 pal(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
    return a + b * cos(6.2831853 * (c * t + d));
}

// Sources for color mappings:
// - https://en.wikipedia.org/wiki/SRGB (conversion matrices from here)
// - https://www.w3.org/Graphics/Color/srgb
// There are *a lot* of slightly different conversion matrices on the
// internet for XYZ to linear srgb. The ones used here should
// at least match the ones in tkn/color.hpp

// Approximations using constant gamma.
// Unless it's ping-pong mapping for just doing something in another
// space (e.g. dithering for quantization), use the real conversion
// functions below.
vec3 toLinearCheap(vec3 nonlinear) {
	return pow(nonlinear, vec3(2.2));
}

vec3 toNonlinearCheap(vec3 linear) {
	return pow(linear, vec3(1 / 2.2));
}

vec3 toNonlinear(vec3 linear) {
	vec3 a = step(linear, vec3(0.0031308));
	vec3 clin = 12.92 * linear;
	vec3 cgamma = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
	return mix(clin, cgamma, a);
}

vec3 toLinear(vec3 nonlinear) {
	vec3 a = step(nonlinear, vec3(0.04045));
	vec3 clin = nonlinear / 12.92;
	vec3 cgamma = pow((nonlinear + 0.055) / 1.055, vec3(2.4));
	return mix(clin, cgamma, a);
}

// convert between XYZ and linear sRGB
// right-handed matrix mulitplication needed since glsl is column-major
vec3 XYZtoRGB(vec3 xyz) {
	return xyz * mat3(
		+3.24096994, -1.53738318, -0.49861076,
		-0.96924364, +1.87596740, +0.04155506,
		+0.05563008, -0.20397696, +1.05697151);
}

vec3 RGBtoXYZ(vec3 rgb) {
	return rgb * mat3(
		0.41239080, 0.35758434, 0.18048079,
		0.21263901, 0.71516868, 0.07219232,
		0.01933082, 0.11919478, 0.95053215);
}
