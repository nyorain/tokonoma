#pragma once

#include "draw.hpp"

namespace rvg2 {

class Texture;
class UpdateContext;
class DeviceObject;


struct Pools {
	VertexPool vertex;
	IndexPool index;
	TransformPool transform;
	ClipPool clip;
	DrawPool draw;
	std::vector<PaintPool> paint;
};

struct DrawBatch {
	UpdateContext* ctx;

	std::vector<DrawCall> calls;
	std::vector<DrawDescriptor> descriptors;
	Pools pools;

	bool updateDevice(UpdateFlags);
};

} // namespace rvg2
