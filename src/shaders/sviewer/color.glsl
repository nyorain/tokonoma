// See http://iquilezles.org/www/articles/palettes/palettes.htm
vec3 pal(float t, vec3 a, vec3 b, vec3 c, vec3 d) {
    return a + b * cos(6.28318 * (c * t + d));
}
