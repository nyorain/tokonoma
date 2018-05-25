#version 450

layout(push_constant) uniform Color {
	vec4 color;
} col;

layout(location = 0) in float in_alpha;
layout(location = 0) out vec4 out_col;

void main() {
	out_col = col.color;

	// make the tip a bit red
	// float cf = max(pow(in_alpha - 0.9, 0.1), 0);
	// out_col.r += 0.2 * cf;
	// out_col.b -= 0.1 * cf;

	out_col.a *= in_alpha;
}
