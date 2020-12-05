layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform usampler2D inR;
layout(set = 0, binding = 1) uniform usampler2D inG;
layout(set = 0, binding = 2) uniform usampler2D inB;

void main() {
	outFragColor.r = textureLod(inR, uv, 0).r / 4294967296.f;
	outFragColor.g = textureLod(inG, uv, 0).r / 4294967296.f;
	outFragColor.b = textureLod(inB, uv, 0).r / 4294967296.f;
	outFragColor.a = 1.0;
}

