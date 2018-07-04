#pragma once

#include "part.hpp"
#include <rvg/paint.hpp>
#include <rvg/shapes.hpp>

namespace parts {

class RenderShape : public Part {
	rvg::Shape shape;
	rvg::Paint* paint {};
};

} // namespace parts
