struct Particle {
	vec3 pos;
	float lifetime;
	vec3 vel;
	float mass;
};

struct UboData {
	mat4 vp;
	vec3 camPos;
	float dt;
	vec3 attrPos;
	float targetZ;
	vec3 camAccel; // writeback
	float attrStrength;
};
