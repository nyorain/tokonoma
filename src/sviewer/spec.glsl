// NOTE (idea): allow shaders to specify own variables that can be changed
// via ui (vui)

layout(set = 0, binding = 0) uniform UBO {
	vec2 mpos; // mouse position, [0, 1] with origin top left
	float time; // time in seconds since last shader reload
	uint effect; // can be increased/decrased. Cycle through effects

	// 3D camera information
	// camera position; feedback for wasd+qe keys
	// y axis going up, z axis coming out of screen
	// could be used raytracing/3d stuff
	vec3 camPos;
	float aspect;
	vec3 camDir;
	float fov;
} ubo;
