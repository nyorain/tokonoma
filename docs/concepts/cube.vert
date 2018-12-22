#version 450

// front: {0, 1, 2,  2, 1, 3}
// right: {1, 5, 3,  3, 5, 7}
// top: {2, 3, 6,  6, 3, 7}
// left: {4, 0, 6,  6, 0, 2}
// bottom: {4, 5, 0,  0, 5, 1}
// back: {5, 4, 7,  7, 4, 6}

void main() {
	// gl_VertexIndex from [0, 8)
	gl_Position = vec4(
		-1 + 2 * ((gl_VertexIndex << 0) & 1),
		-1 + 2 * ((gl_VertexIndex << 1) & 1),
		-1 + 2 * ((gl_VertexIndex << 2) & 1),
		1.0);
}
