// http://www.iquilezles.org/www/articles/warp/warp.htm
float water1(in vec2 p, float time, out vec2 q, out vec2 r) {
	q.x = fbm(p + vec2(time,0.0));
	q.y = fbm(p + vec2(5.2,1.3));

	r.x = fbm(p + 4.0*q + vec2(1.7,9.2));
	r.y = fbm(p + 4.0*q + vec2(8.3,2.8));

	return fbm(p + 4.0 * r);
}

