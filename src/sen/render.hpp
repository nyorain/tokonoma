#pragma once

#include "geometry.hpp"
#include <vpp/sharedBuffer.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/image.hpp>
#include <vpp/trackedDescriptor.hpp>

struct RenderBox {
	Box box;
	nytl::Vec4f color;

	// referenced here in case it needs update
	std::size_t bufOff; // raytrace: transform, color

	// data for rasterized rendering
	vpp::SubBuffer rasterdata; // raster: inv transform, normal transform, color
	vpp::TrDs ds;

	// TODO
	vpp::ViewableImage global; // ray traced global lightning stuff; cubemap
};

// TODO: concentrated on boxes for now
struct RenderSphere {
	Sphere sphere;
	vpp::SubBuffer transform;
	vpp::ViewableImage global;
	vpp::TrDs ds;
};
