struct Camera {
	nytl::Vec3f pos;
	Quaternion rot;
	bool update;
};

// camera controllers
// spaceship: no fixed up firection, uses quaternion for orientation.
// might look weird when rotating on ground since up isn't fixed.
struct SpaceshipCamCon {
};

struct FPCamCon {
	float yaw {0.f};
	float pitch {0.f};
};

struct TrackballCamCon {
	float zoom {1.f};
};
