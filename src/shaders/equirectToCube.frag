#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform UBO {
	vec3 x;
	float _1; // padding
	vec3 y;
	float _2; // padding
	vec3 z; // normal, cube map face
} dir;

layout(set = 0, binding = 1) uniform sampler2D equirectangularMap;

const vec2 invAtan = vec2(0.1591, 0.3183);
vec2 equirectTexCoord(vec3 v) {
    vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
    uv *= invAtan;
    uv += 0.5;
    return uv;
}

void main() {		
	vec2 uv = 2 * inUV.xy - 1; // normalize to [-1, 1]
	vec3 pos = normalize(dir.z + uv.x * dir.x + uv.y * dir.y);
    uv = equirectTexCoord(pos);
    vec3 color = texture(equirectangularMap, uv).rgb;
    outColor = vec4(color, 1.0);
}
