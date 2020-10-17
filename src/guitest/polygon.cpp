#include "polygon.hpp"
#include <dlg/dlg.hpp>
#include <katachi/stroke.hpp>

namespace rvg2 {

// util
template<typename T>
void write(nytl::Span<T>& span, const T& value) {
	dlg_assert(!span.empty());
	span[0] = value;
	span = span.subspan(1);
}

Polygon::Polygon(IndexPool& ip, VertexPool& vp) : indexPool_(&ip), vertexPool_(&vp) {
}

Polygon::~Polygon() {
	if(!indexPool_ || !vertexPool_) {
		dlg_assert(!vertexPool_ && !indexPool_);
		return;
	}
}

void swap(Polygon& a, Polygon& b) {
	using std::swap;

	auto swapDraw = [](Polygon::Draw& a, Polygon::Draw& b) {
		swap(a.indexCount, b.indexCount);
		swap(a.vertexCount, b.vertexCount);
		swap(a.vertexStart, b.vertexStart);
		swap(a.indexStart, b.indexStart);
	};

	swapDraw(a.fill_, b.fill_);
	swapDraw(a.stroke_, b.stroke_);
	swapDraw(a.fillAA_, b.fillAA_);
	swap(a.strokeWidth_, b.strokeWidth_);
	swap(a.indexPool_, b.indexPool_);
	swap(a.vertexPool_, b.vertexPool_);
}

void Polygon::update(Span<const Vec2f> points, const DrawMode& dm) {
	dlg_assert(indexPool_);
	dlg_assert(vertexPool_);
	dlg_assert(dm.fill || dm.stroke);
	dlg_assert(dm.stroke >= 0.f);
	dlg_assert((!(dm.fill && dm.color.fill) && !(dm.stroke && dm.color.stroke)) ||
		dm.color.points.size() == points.size());

	auto convertVertex = [](const ktc::Vertex& vtx) {
		Vertex dst;
		dst.color = vtx.color;
		dst.uv = vtx.aa;
		dst.pos = vtx.position;
		return dst;
	};

	// TODO(opt): only update changed parts, not always full polygon
	// update fill
	if(dm.fill) {

		// Effectively, the draw call here is split into two:
		// First a triangle-fan to fill the polygon (slightly inset).
		// Then a 1px stroke around the polygon (fixing the previous inset,
		// adding AA on the edges).
		// Allocating everything in a single allocation, with a single draw
		// command allows us to return a single DrawInstance for both
		// and furthermore reuse the fill vertices for stroking as well.
		if(dm.aaFill) {
			auto color = nytl::span(dm.color.points);
			if(!dm.color.fill) {
				color = {};
			}

			// TODO: fix handling of looping
			auto fill = ktc::bakeCombinedFillAA(points, color, 1.f);

			vertexPool_->reallocRef(fill_.vertexStart, fill_.vertexCount, fill.vertices.size());
			indexPool_->reallocRef(fill_.indexStart, fill_.indexCount, fill.indices.size());

			auto verts = vertexPool_->writable(fill_.vertexStart, fill_.vertexCount);
			auto inds = indexPool_->writable(fill_.indexStart, fill_.indexCount);

			for(auto& v : fill.vertices) {
				write(verts, convertVertex(v));
			}

			for(auto& i : fill.indices) {
				write(inds, i);
			}

			// should be fully filled
			dlg_assert(verts.empty());
			dlg_assert(inds.empty());
		} else {
			vertexPool_->reallocRef(fill_.vertexStart, fill_.vertexCount, points.size());
			indexPool_->reallocRef(fill_.indexStart, fill_.indexCount, 3 * points.size());
			auto verts = vertexPool_->writable(fill_.vertexStart, fill_.vertexCount);
			auto inds = indexPool_->writable(fill_.indexStart, fill_.indexCount);

			for(auto i = 0u; i < points.size(); ++i) {
				Vertex vtx;
				vtx.pos = points[i];
				vtx.uv = {0.f, 0.f};
				vtx.color = dm.color.fill ? dm.color.points[i] : Vec4u8{0, 0, 0, 0};
				write(verts, vtx);
			}

			// should be fully filled
			dlg_assert(verts.empty());
			ktc::triangleFanIndices(inds, fill_.vertexCount);
		}

	}

	// update stroke
	if(dm.stroke) {
		std::vector<Vertex> vts;
		auto writer = [&](const ktc::Vertex& vtx) {
			vts.push_back(convertVertex(vtx));
		};

		auto color = nytl::span(dm.color.points);
		if(!dm.color.stroke) {
			color = {};
		}

		ktc::StrokeSettings settings;
		settings.capFringe = dm.aaStroke ? 1.f : 0.f;
		settings.width = dm.stroke;
		settings.loop = dm.loop;

		ktc::bakeStroke(points, settings, color, writer);

		vertexPool_->reallocRef(stroke_.vertexStart, stroke_.vertexCount, vts.size());
		indexPool_->reallocRef(stroke_.indexStart, stroke_.indexCount, 3 * vts.size());

		vertexPool_->write(stroke_.vertexStart, vts);

		auto inds = indexPool_->writable(stroke_.indexStart, stroke_.indexCount);
		ktc::triangleStripIndices(inds, vts.size());
	}
}

std::array<DrawInstance, 2> Polygon::fill(DrawRecorder& rec) const {
	dlg_assertm(fill_.vertexCount, "Polygon can't be filled");

	std::array<DrawInstance, 2> ret;
	ret[0] = rec.draw(fill_.indexStart, fill_.indexCount, fill_.vertexStart,
		DrawType::fill, 0.f);
	if(fillAA_.vertexCount) {
		ret[1] = rec.draw(fillAA_.indexStart, fillAA_.indexCount, fillAA_.vertexStart,
			DrawType::stroke, 1.f);
	}

	return ret;
}

DrawInstance Polygon::stroke(DrawRecorder& rec) const {
	dlg_assertm(stroke_.vertexCount, "Polygon can't be stroked");
	return rec.draw(stroke_.indexStart, stroke_.indexCount, stroke_.vertexStart,
		DrawType::stroke, 1.f);
}

} // namespace rvg2
