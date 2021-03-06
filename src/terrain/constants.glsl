// const vec3 lightColor = vec3(0.88, 0.85, 0.8);
// const vec3 ambientColor = vec3(0.30, 0.37, 0.55);

const float allMin = -.0;
const float octaveMin = -0.25;

const float depositionRate = 0.2;
const float erosionRate = 0.4;
const float inertia = 0.05;
const float evaporation = 0.01;
const float minWater = 0.0001;
const float minSlope = 0.0001;
const float capacity = 0.05;
const float gravity = 200.0;

const float erosionRadius = 8.f;

struct UboData {
	mat4 viewMtx;
	mat4 projMtx;
	mat4 viewProjMtx;

	mat4 invViewMtx;
	mat4 invProjMtx;
	mat4 invViewProjMtx;

	vec3 viewPos;
	float dt;
	vec3 toLight;
	float time;
	vec3 sunColor;
	uint frameCounter;
	vec3 ambientColor;
};
