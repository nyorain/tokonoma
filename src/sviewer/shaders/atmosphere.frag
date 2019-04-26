#version 450

layout(location = 0) in vec2 inuv;
layout(location = 0) out vec4 outcol;

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos;
	float time;
} ubo;

vec3 shadeBg(vec2 uv, vec3 nml, float x, float y) {
    vec3 bgLight = normalize(vec3(x, y, -1.f));
    float sunD = dot(bgLight, nml) > 0.995 ? 1.0 : 0.0;
	vec3 sun = vec3(6.5, 3.5, 2.0);
	float skyPow = dot(nml, vec3(0.0, -1.0, 0.0));
    float centerPow = 0.0; //-dot(uv,uv);
    float horizonPow = pow(1.0-abs(skyPow), 3.0)*(5.0+centerPow);
	float sunPow = dot(nml, bgLight);
	float sp = max(sunPow, 0.0);
    float scattering = clamp(1.0 - abs(2.0*(-bgLight.y)), 0.0, 1.0);
	vec3 bgCol = max(0.0, skyPow)*2.0*vec3(0.8);
	bgCol += 0.5*vec3(0.8)*(horizonPow);
	bgCol += sun*(sunD+pow( sp, max(128.0, abs(bgLight.y)*512.0) ));
	bgCol += vec3(0.4,0.2,0.15)*(pow( sp, 8.0) + pow( sp, max(8.0, abs(bgLight.y)*128.0) ));
    bgCol *= mix(vec3(0.7, 0.85, 0.95), vec3(1.0, 0.45, 0.1), scattering);
    bgCol *= 1.0 - clamp(bgLight.y*3.0, 0.0, 0.6);

	return pow(max(vec3(0.0), bgCol), vec3(2.6));
}

void main() {
	vec2 uv = 2 * inuv - 1;
	uv.y *= -1;

	vec3 d = normalize(vec3(uv, 1.f));

	float ptime = 0.1 * ubo.time - 1.f;
	float x = 2 * ubo.mpos.x - 1.f;
	float y = 2 * ubo.mpos.y - 1.f;
    vec3 col = shadeBg(uv, -d, -x, y);

	// low pow factor (e.g. 0.5): misty effect
    outcol = pow(vec4((1.0 - exp(-1.3 * col.rgb)), 1.0 ), vec4(2.5));
}
