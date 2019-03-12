// See http://iquilezles.org/www/articles/palettes/palettes.htm
// a: color offset (base color) [0.5, 0.5, 0.5]
// b: cos to color; a ± b must be in [0, 1]; [0.5, 0.5, 0.5]
// c: how fast t is moving for the different components [1, 1, 1]
// d: cos offset [0, 0.25, 0.5]
vec3 pal(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
    return a + b * cos(6.2831853 * (c * t + d));
}
