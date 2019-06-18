#version 450

layout(location = 0) out vec4 fragColor;

void main() {
	// constant color
	// TODO: could make that specialization constant i guess
	fragColor = vec4(1.0);
}
