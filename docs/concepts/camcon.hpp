// almightly camera class concept.
// Combines all camera types into a common easy-to-use hight-level interface

namespace tkn {

class CameraController {
public:
	enum Flags {
		quaternion,
		quatStaticUp,
		quatRoll,
		touch,
		touchVisualize,
	};

public:
	CameraMovement movement;
	float yawMult = 1.f;
	float pitchMult = 1.f;

public:
	bool update(swa_display* dpy, double dt);
	void touchBegin(unsigned id, nytl::Vec2f pos);
	void touchUpdate(unsigned id, nytl::Vec2f pos);
	void touchEnd(unsigned id);
	void touchCancel();

	void startViewRotation(bool, nytl::Vec2f pos);
	void rotateView(nytl::Vec2f pos);

	bool changed() const;
	bool clearChanged();

	nytl::Mat4f projectionMatrix() const;
	nytl::Mat4f viewMatrix() const;
	nytl::Mat4f vpMatrix() const;

protected:
	Flags flags_;
	union {
		Camera cam_;
		QuatCam qcam_;
	};

	struct {
		float near;
		float far;
		float fov;
		float aspect;
	} proj_;

	TouchCameraController touch_;
};

} // namespace tkn
