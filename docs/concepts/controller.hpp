namespace tkn {

struct ShaceshipCameraController {
	float yawFac = 0.01;
	float pitchFac = 0.01;
	float rollFac = 0.01;

	bool rotating_;
	nytl::Vec2f mposStart_;
	nytl::Vec2f mpos_;
	QuatCamera* camera_;

	struct {
		float pitch {0.f};
		float yaw {0.f};
		float roll {0.f};
	} vel_;
};

}
