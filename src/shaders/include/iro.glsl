// symbolic special constants
const uint none = 0xFFFFFFFFu;
const float oneSixth = 0.166;

// buildings
const uint FieldEmpty = 0u; // empty field
const uint FieldResource = 1u; // resource harvester
const uint FieldSpawn = 2u; // spawns strength
const uint FieldTower = 3u; // damages nearby enemies
const uint FieldAccel = 4u; // accelerates

// sides, order of Field.next
const uint SideRight = 0u;
const uint SideTopRight = 1u;
const uint SideTopLeft = 2u;
const uint SideLeft = 3u;
const uint SideBotLeft = 4u;
const uint SideBotRight = 5u;
const float sqrt2 = 1.41421356237;
const float isqrt2 = 0.70710678118;
const vec2 sideDirections[6] = {
	vec2(1, 0), // right
	vec2(isqrt2, isqrt2), // topRight
	vec2(-isqrt2, isqrt2), // topLeft
	vec2(-1, 0), // left
	vec2(-isqrt2, -isqrt2), // botLeft
	vec2(isqrt2, -isqrt2), // botRight
};

// Returns the logical direction of a SideXXX value
vec2 direction(uint side) {
	return sideDirections[side];
}

bool isBuilding(uint fieldType) {
	return fieldType != FieldEmpty;
}

// TODO: we probably shouldn't store pos here. Only needed in vertex
// shader. Will never be changed in compute shader.
struct Field {
	vec2 _pos;
	uint type;
	float strength; // in [0, 1] for empty cells (?)
	vec2 velocity;
	uint player;
	uint next[6]; // direct neighbors
	float _;
};
