// http://filmicworlds.com/blog/filmic-tonemapping-operators/
// has a nice preview of different tonemapping operators
vec3 uncharted2map(vec3 x) {
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}

vec3 uncharted2tonemap(vec3 x) {
	const float W = 11.2; // whitescale
	x = uncharted2map(x);
	return x * (1.f / uncharted2map(vec3(W)));
}

// Hejl Richard tone map
// http://filmicworlds.com/blog/filmic-tonemapping-operators/
vec3 hejlRichardTonemap(vec3 color) {
    color = max(vec3(0.0), color - vec3(0.004));
    return pow((color*(6.2*color+.5))/(color*(6.2*color+1.7)+0.06), vec3(2.2));
}

// ACES tone map
// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
vec3 acesTonemap(vec3 color) {
    const float A = 2.51;
    const float B = 0.03;
    const float C = 2.43;
    const float D = 0.59;
    const float E = 0.14;
    return clamp((color * (A * color + B)) / (color * (C * color + D) + E), 0.0, 1.0);
}
