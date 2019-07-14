// http://www.thetenthplanet.de/archives/1180
// dFdx/dFdy only defined in fragment shader

// normal: geometry normal (interpolated normal from vertices)
// tnormal: local tangen-space normal, read from texture (but in [-1, 1] range)
vec3 tbnNormal(vec3 normal, vec3 pos, vec2 uv, vec3 tnormal) {
	vec3 q1 = dFdx(pos);
	vec3 q2 = dFdy(pos);
	vec2 st1 = dFdx(uv);
	vec2 st2 = dFdy(uv);

	vec3 p1 = cross(q2, normal);
	vec3 p2 = cross(normal, q1);
	vec3 t = p2 * st1.x + p1 * st2.x;
	vec3 b = p2 * st1.y + p1 * st2.y;

	float scale = inversesqrt(max(dot(t, t), dot(b, b)));
	return normalize(mat3(scale * t, scale * b, normal) * tnormal);
}

// compute world space normal
vec3 tbnNormal(vec3 normal, vec3 pos, vec2 uv, sampler2D normalTex) {
	// texture contains normal in tangent space
	// we could also just use a signed format here i guess
	return tbnNormal(normal, pos, uv, texture(normalTex, uv).xyz * 2.0 - 1.0);
}
