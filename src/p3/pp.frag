layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outFragColor;

layout(set = 0, binding = 0) uniform sampler2D inColor;

void main() {
	outFragColor = textureLod(inColor, uv, 0);
	outFragColor.a = 1.0;
}
