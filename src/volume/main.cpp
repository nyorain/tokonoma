// TODO: implement dual marching cubes. Table already in tables.hpp
// TODO: implement grid-based normals
//   (and some other stuff fixed in gdv project but not yet here)
// TODO: add vec and mat hashing to nytl. Check first what the license/
//   permissions on it is

#include "tables.hpp"

#include <tkn/singlePassApp.hpp>
#include <tkn/window.hpp>
#include <tkn/bits.hpp>
#include <tkn/ccam.hpp>
#include <tkn/render.hpp>
#include <tkn/types.hpp>

#include <vpp/vk.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/queue.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <dlg/dlg.hpp>

#include <rvg/paint.hpp>
#include <rvg/shapes.hpp>
#include <rvg/context.hpp>

#include <unordered_map>
#include <array>

#include <shaders/volume.volume.vert.h>
#include <shaders/volume.volume.frag.h>

using namespace tkn::types;

struct Cell {
	std::array<Vec3f, 8> pos;
	std::array<float, 8> val;
};

struct Triangle {
	std::array<Vec3f, 3> pos;
};

// vec hashing taken from glm (licensed MIT)
// https://glm.g-truc.net
inline void hash_combine(size_t &seed, size_t hash) {
	hash += 0x9e3779b9 + (seed << 6) + (seed >> 2);
	seed ^= hash;
}

namespace std {
template<std::size_t I, typename T>
struct hash<nytl::Vec<I, T>> {
	std::size_t operator()(const nytl::Vec<I, T>& v) const {
		size_t seed = 0;
		hash<T> hasher;
		for(auto& val : v) {
			hash_combine(seed, hasher(val));
		}
		return seed;
	}
};

}

struct Volume {
	static constexpr auto size = Vec3ui{64, 64, 64};
	static constexpr auto extent = Vec3f{3, 3, 3};
	static constexpr auto start = -0.5f * extent;
	static constexpr auto scale = Vec3f{
		extent.x / size.x,
		extent.y / size.y,
		extent.z / size.z};

	std::vector<float> sdf;
	std::vector<Vec3f> positions;
	std::vector<Vec3f> normals;
	std::vector<u32> indices;

	u32 id(u32 x, u32 y, u32 z) const {
		return z * size.x * size.y + y * size.x + x;
	}

	u32 id(Vec3ui pos) const {
		return id(pos.x, pos.y, pos.z);
	}

	// < 0: outside
	// > 0: inside
	float func(float x, float y, float z) {
		const float w = 1.f;
		const float r = 0.5 * (1 + std::sqrt(5));
		auto t = (x * x + y * y + z * z - w * w);
		auto val = 4 * (r * r * x * x - y * y) *
			(r * r * y * y - z * z) *
			(r * r * z * z - x * x) -
			(1 + 2 * r) * t * t * w * w;
		return val;
	}

	Vec3f normal(float x, float y, float z) {
		const float w = 1.f;
		const float r = 0.5 * (1 + std::sqrt(5));

		auto d = [w, r](float x, float y, float z) {
			auto t = (x * x + y * y + z * z - w * w);
			auto dt = 2 * x;
			return 4 * (r * r * y * y - z * z)
				* ((r * r * 2 * x) * (r * r * z * z - x * x) + (r * r * x * x - y * y) * (-2) * x)
				- (1 + 2 * r) * (2 * dt * t) * w * w;
		};

		return -1.f * normalized(Vec3f{d(x, y, z), d(y, z, x), d(z, x, y)});
	}

	Vec3f normal(Vec3f v) { return normal(v.x, v.y, v.z); }
	float func(Vec3f v) { return func(v.x, v.y, v.z); }

	// ceil
	// undefined outside of grid
	Vec3ui posToGrid(Vec3f pos) {
		using namespace nytl::vec::cw::operators;
		return Vec3ui((pos - start) / scale);

	}

	Vec3f posToGridf(Vec3f pos) {
		using namespace nytl::vec::cw::operators;
		return Vec3f((pos - start) / scale);
	}

	float value(Vec3f p) {
		auto gp = nytl::vec::cw::clamp(posToGridf(p), Vec3f{0, 0, 0},
			Vec3f{size.x - 1.f, size.y - 1.f, size.z - 1.f});
		Vec3ui id = Vec3ui(gp);

		float fx = gp.x - id.x;
		float fy = gp.y - id.y;
		float fz = gp.z - id.z;
		auto c = cell(id);

		// trilinear interpolation
		float res = 0.f;
		res += (1 - fx) * (1 - fy) * (1 - fz) * c.val[0];
		res += fx * (1 - fy) * (1 - fz) * c.val[1];
		res += fx * (1 - fy) * fz * c.val[2];
		res += (1 - fx) * (1 - fy) * fz * c.val[3];
		res += (1 - fx) * fy * (1 - fz) * c.val[4];
		res += fx * fy * (1 - fz) * c.val[5];
		res += fx * fy * fz * c.val[6];
		res += (1 - fx) * fy * fz * c.val[7];

		return res;
	}

	Vec3f sdfNormal(Vec3f pos) {
		auto gid = posToGrid(pos);
		for(auto i = 0u; i < 3; ++i) {
			gid[i] = std::min(gid[i], size[i] - 2u);
		}

		nytl::Vec3f res;
		for(auto i = 0u; i < 3; ++i) {
			auto p1 = pos;
			auto p2 = pos;
			// central difference
			p1[i] = p1[i] - scale[i];
			p2[i] = p2[i] + scale[i];
			res[i] = (value(p2) - value(p1)) / (2 * scale[i]);
		}

		return -1.f * normalized(res);
	}

	void generateSDF() {
		sdf.resize(size.x * size.y * size.z);

		for(auto iz = 0u; iz < size.z; ++iz) {
			auto z = start.z + (iz / float(size.z)) * extent.z;
			for(auto iy = 0u; iy < size.y; ++iy) {
				auto y = start.y + (iy / float(size.y)) * extent.y;
				for(auto ix = 0u; ix < size.x; ++ix) {
					auto x = start.x + (ix / float(size.x)) * extent.x;
					sdf[id(ix, iy, iz)] = func(x, y, z);
				}
			}
		}
	}

	unsigned cellConfig(const Cell& c, float iso) {
		unsigned ret = 0u;
		for(auto i = 0u; i < 8; ++i) {
			if(c.val[i] < iso) {
				ret |= (1u << i);
			}
		}
		return ret;
	}

	Vec3f interpolate(Vec3f a, Vec3f b, float va, float vb, float iso) {
		if(va > vb) {
			using std::swap;
			swap(va, vb);
			swap(a, b);
		}

		auto f = std::clamp((iso - va) / (vb - va), 0.f, 1.f);

		// snapping
		// if(f < 0.2) {
		// 	f = 0.f;
		// } else if(f > 0.8) {
		// 	f = 1.f;
		// }

		return a + f * (b - a);
	}

	Vec3f interpolate(const Cell& g, unsigned i1, unsigned i2, float iso) {
		return interpolate(g.pos[i1], g.pos[i2], g.val[i1], g.val[i2], iso);
	}

	unsigned int triangulate(const Cell& cell, float iso,
			std::array<Triangle, 5>& tris) {
		auto cubeindex = cellConfig(cell, iso);

		auto edges = edgeTable[cubeindex];
		if(edges == 0) { // no surfaces to generate
			return 0;
		}

		std::array<Vec3f, 12> verts;
		for(auto i = 0u; i < 12; ++i) {
			if(edges & (1 << i)) {
				auto& points = edgePoints[i];
				verts[i] = interpolate(cell, points.a, points.b, iso);
			} else {
				verts[i] = {-1.f, -1.f, -1.f};
			}
		}

		auto n = 0u;
		for(auto i = 0u; triTable[cubeindex][i] != -1; i += 3) {
			// flip order, table contains it clockwise
			tris[n].pos[0] = verts[triTable[cubeindex][i + 0]];
			tris[n].pos[1] = verts[triTable[cubeindex][i + 2]];
			tris[n].pos[2] = verts[triTable[cubeindex][i + 1]];
			for(auto i = 0u; i < 3; ++i) {
				dlg_assert((tris[n].pos[i] != nytl::Vec3f{-1.f, -1.f, -1.f}));
			}
			++n;
		}

		return n;
	}

	Cell cell(Vec3ui pos) {
		// see corner sketch in mcLookUp.h
		// z assumed to "come out of the screen"
		Cell cell {};
		for(auto i = 0u; i < 8; ++i) {
			Vec3ui off = Vec3ui {((i + 1) / 2) % 2, i / 4, (i / 2) % 2};
			auto p = start + nytl::vec::cw::multiply(scale, pos + Vec3f(off));
			cell.pos[i] = p;
			cell.val[i] = sdf[id(pos + off)];
		}
		return cell;
	}

	void marchingCubes(float iso) {
		auto total = 0u;
		for(auto z = 0u; z + 1 < size.z; ++z) {
			for(auto y = 0u; y + 1 < size.y; ++y) {
				for(auto x = 0u; x + 1 < size.x; ++x) {
					auto cell = this->cell({x, y, z});

					std::array<Triangle, 5> tris;
					auto count = triangulate(cell, iso, tris);
					for(auto i = 0u; i < count; ++i) {
						auto& tri = tris[i];
						if(tri.pos[0] == tri.pos[1] ||
								tri.pos[0] == tri.pos[2] ||
								tri.pos[1] == tri.pos[2]) {
							continue;
						}

						positions.push_back(tri.pos[0]);
						positions.push_back(tri.pos[1]);
						positions.push_back(tri.pos[2]);
						normals.push_back(normal(tri.pos[0]));
						normals.push_back(normal(tri.pos[1]));
						normals.push_back(normal(tri.pos[2]));
						// normals.push_back(sdfNormal(tri.pos[0]));
						// normals.push_back(sdfNormal(tri.pos[1]));
						// normals.push_back(sdfNormal(tri.pos[2]));
						++total;
					}
				}
			}
		}

		dlg_info("{} Triangles generated", total);

		// remove duplicates
		// we hash and compare float values here but since the interpolation
		// for two adjacent cubes work on the exact same floating point
		// numbers they should return the same result and this should work.
		indices.reserve(positions.size()); // conservative approx
		std::unordered_map<Vec3f, int> seen;
		auto write = 0u;
		for(auto i = 0u; i < positions.size(); ++i) {
			if(auto it = seen.find(positions[i]); it != seen.end()) {
				indices.push_back(it->second);
			} else {
				seen[positions[i]] = write;
				indices.push_back(write);
				positions[write] = positions[i];
				normals[write] = normals[i];
				++write;
			}
		}

		positions.resize(write);
		normals.resize(write);
	}
};

class VolumeApp : public tkn::SinglePassApp {
public:
	using Base = tkn::SinglePassApp;

public:
	bool init(nytl::Span<const char*> args) override {
		if(!Base::init(args)) {
			return false;
		}

		rvgInit();
		auto& dev = vkDevice();
		auto bindings = std::array{
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment)
		};

		dsLayout_.init(dev, bindings);
		pipeLayout_ = {dev, {{dsLayout_.vkHandle()}}, {}};

		vpp::ShaderModule vertShader{dev, volume_volume_vert_data};
		vpp::ShaderModule fragShader{dev, volume_volume_frag_data};
		vpp::GraphicsPipelineInfo gpi {renderPass(), pipeLayout_, {{{
			{vertShader, vk::ShaderStageBits::vertex},
			{fragShader, vk::ShaderStageBits::fragment},
		}}}, 0, samples()};

		gpi.assembly.topology = vk::PrimitiveTopology::triangleList;
		gpi.depthStencil.depthTestEnable = true;
		gpi.depthStencil.depthWriteEnable = true;
		gpi.depthStencil.depthCompareOp = vk::CompareOp::lessOrEqual;

		// TODO: enable culling
		gpi.rasterization.cullMode = vk::CullModeBits::none;
		gpi.rasterization.frontFace = vk::FrontFace::counterClockwise;

		vk::VertexInputAttributeDescription attribs[2];
		attribs[0].format = vk::Format::r32g32b32Sfloat;

		attribs[1].format = vk::Format::r32g32b32Sfloat;
		attribs[1].binding = 1;
		attribs[1].location = 1;

		vk::VertexInputBindingDescription bufs[2];
		bufs[0].inputRate = vk::VertexInputRate::vertex;
		bufs[0].stride = sizeof(Vec3f);
		bufs[1].inputRate = vk::VertexInputRate::vertex;
		bufs[1].stride = sizeof(Vec3f);
		bufs[1].binding = 1;

		gpi.vertex.pVertexAttributeDescriptions = attribs;
		gpi.vertex.vertexAttributeDescriptionCount = 2;
		gpi.vertex.pVertexBindingDescriptions = bufs;
		gpi.vertex.vertexBindingDescriptionCount = 2;

		pipe_ = {dev, gpi.info()};

		auto uboSize = sizeof(nytl::Mat4f) + sizeof(Vec3f);
		cameraUbo_ = {dev.bufferAllocator(), uboSize,
			vk::BufferUsageBits::uniformBuffer, dev.hostMemoryTypes()};
		ds_ = {dev.descriptorAllocator(), dsLayout_};

		vpp::DescriptorSetUpdate dsu(ds_);
		dsu.uniform({{{cameraUbo_}}});
		dsu.apply();

		// upload data
		Volume volume;
		volume.generateSDF();
		volume.marchingCubes(0.0);

		dlg_assert(volume.positions.size() == volume.normals.size());
		auto vsize = sizeof(Vec3f) * volume.positions.size();
		positions_ = {dev.bufferAllocator(), vsize,
			vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::transferDst, dev.deviceMemoryTypes()};
		normals_ = {dev.bufferAllocator(), vsize,
			vk::BufferUsageBits::vertexBuffer |
			vk::BufferUsageBits::transferDst, dev.deviceMemoryTypes()};
		indices_ = {dev.bufferAllocator(), sizeof(u32) * volume.indices.size(),
			vk::BufferUsageBits::indexBuffer |
			vk::BufferUsageBits::transferDst, dev.deviceMemoryTypes()};
		indexCount_ = volume.indices.size();

		auto& qs = dev.queueSubmitter();
		auto qfam = qs.queue().family();
		auto cb = dev.commandAllocator().get(qfam);
		vk::beginCommandBuffer(cb, {});

		auto _s1 = vpp::fillStaging(cb, positions_, tkn::bytes(volume.positions));
		auto _s2 = vpp::fillStaging(cb, normals_, tkn::bytes(volume.normals));
		auto _s3 = vpp::fillStaging(cb, indices_, tkn::bytes(volume.indices));

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		// tkn::init(touch_, camera_, rvgContext());
		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		tkn::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		vk::cmdBindVertexBuffers(cb, 0,
			{{positions_.buffer().vkHandle(), normals_.buffer().vkHandle()}},
			{{positions_.offset(), normals_.offset()}});
		vk::cmdBindIndexBuffer(cb, indices_.buffer(), indices_.offset(),
			vk::IndexType::uint32);
		vk::cmdDrawIndexed(cb, indexCount_, 1, 0, 0, 0);

		// if(touch_.alt) {
		// 	rvgContext().bindDefaults(cb);
		// 	rvgWindowTransform().bind(cb);
		// 	touch_.paint.bind(cb);
		// 	touch_.move.circle.fill(cb);
		// 	touch_.rotate.circle.fill(cb);
		// }
	}

	void update(double dt) override {
		Base::update(dt);
		camera_.update(swaDisplay(), dt);
		if(camera_.needsUpdate) {
			Base::scheduleRedraw();
		}
	}

	void updateDevice() override {
		Base::updateDevice();

		if(camera_.needsUpdate) {
			camera_.needsUpdate = false;
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			auto mat = camera_.viewProjectionMatrix();
			tkn::write(span, mat);
			tkn::write(span, camera_.position());
			map.flush();
		}
	}

	void resize(unsigned w, unsigned h) override {
		Base::resize(w, h);
		camera_.aspect({w, h});
	}

	void mouseMove(const swa_mouse_move_event& ev) override {
		Base::mouseMove(ev);
		camera_.mouseMove(swaDisplay(), {ev.dx, ev.dy}, windowSize());
	}

	/*
	bool touchBegin(const swa_touch_event& ev) override {
		if(Base::touchBegin(ev)) {
			return true;
		}

		auto pos = nytl::Vec2f{float(ev.x), float(ev.y)};
		tkn::touchBegin(touch_, ev.id, pos, windowSize());
		return true;
	}

	void touchUpdate(const swa_touch_event& ev) override {
		tkn::touchUpdate(touch_, ev.id, {float(ev.x), float(ev.y)});
		Base::scheduleRedraw();
	}

	bool touchEnd(unsigned id) override {
		if(Base::touchEnd(id)) {
			return true;
		}

		tkn::touchEnd(touch_, id);
		Base::scheduleRedraw();
		return true;
	}
	*/

	const char* name() const override { return "volume"; }
	bool needsDepth() const override { return true; }

protected:
	vpp::Pipeline pipe_;
	vpp::TrDsLayout dsLayout_;
	vpp::TrDs ds_;
	vpp::PipelineLayout pipeLayout_;

	unsigned indexCount_ {};
	vpp::SubBuffer positions_;
	vpp::SubBuffer normals_;
	vpp::SubBuffer indices_;
	vpp::SubBuffer cameraUbo_;
	tkn::ControlledCamera camera_;
};

int main(int argc, const char** argv) {
	return tkn::appMain<VolumeApp>(argc, argv);
}

