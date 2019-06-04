// dFdx/dFdy only defined in fragment shader
// compute world space normal
vec3 tbnNormal(vec3 normal, vec3 pos, vec2 uv, sampler2D normalTex) {
	// http://www.thetenthplanet.de/archives/1180
	vec3 q1 = dFdx(pos);
	vec3 q2 = dFdy(pos);
	vec2 st1 = dFdx(uv);
	vec2 st2 = dFdy(uv);

	vec3 t = normalize(q1 * st2.t - q2 * st1.t);
	vec3 b = -normalize(cross(normal, t));

	// texture contains normal in tangent space
	// we could also just use a signed format here i guess
	vec3 tn = texture(normalTex, uv).xyz * 2.0 - 1.0;
	return normalize(mat3(t, b, normal) * tn);
}

vec3 tbnNormal(vec3 normal, vec3 pos, vec2 uv, vec3 texNormal) {
	// http://www.thetenthplanet.de/archives/1180
	vec3 q1 = dFdx(pos);
	vec3 q2 = dFdy(pos);
	vec2 st1 = dFdx(uv);
	vec2 st2 = dFdy(uv);

	vec3 t = normalize(q1 * st2.t - q2 * st1.t);
	vec3 b = -normalize(cross(normal, t));

	// texture contains normal in tangent space
	// we could also just use a signed format here i guess
	vec3 tn = texNormal * 2.0 - 1.0;
	return normalize(mat3(t, b, normal) * tn);
}
