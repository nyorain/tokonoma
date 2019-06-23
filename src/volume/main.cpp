// TODO: implement dual marching cubes. Table already in tables.hpp
// TODO: maybe use flat normals for normal marching cubes? looks
//   not good with current approach

#include "tables.hpp"

#include <stage/app.hpp>
#include <stage/window.hpp>
#include <stage/bits.hpp>
#include <stage/camera.hpp>
#include <stage/render.hpp>
#include <stage/types.hpp>

#include <vpp/vk.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/trackedDescriptor.hpp>
#include <vpp/bufferOps.hpp>
#include <vpp/sharedBuffer.hpp>
#include <vpp/queue.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <dlg/dlg.hpp>
#include <ny/keyboardContext.hpp>
#include <ny/appContext.hpp>
#include <ny/mouseButton.hpp>

#include <unordered_map>

#include <shaders/volume.volume.vert.h>
#include <shaders/volume.volume.frag.h>

using namespace doi::types;

struct Cell {
	std::array<Vec3f, 8> pos;
	std::array<float, 8> val;
};

struct Triangle {
	std::array<Vec3f, 3> pos;
};

// vec hashing taken from glm
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
	static constexpr auto scale = Vec3f{1.f / size.x, 1.f / size.y, 1.f / size.z};

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

	void generateSDF() {
		sdf.resize(size.x * size.y * size.z);

		// < 0: outside
		// > 0: inside
		const float w = 1.f;
		const float r = 0.5 * (1 + std::sqrt(5));
		auto func = [&](float x, float y, float z){
			auto t = (x * x + y * y + z * z - w * w);
			float val = 4 * (r * r * x * x - y * y) *
				(r * r * y * y - z * z) *
				(r * r * z * z - x * x) -
				(1 + 2 * r) * t * t * w * w;
			return val;
		};

		// auto func = [&](float x, float y, float z) {
		// 	return x * x + y * y + z * z - 1;
		// };

		auto extent = 4.f;
		float start = -extent / 2.f;
		for(auto iz = 0u; iz < size.z; ++iz) {
			auto z = start + extent * (iz / float(size.z));
			for(auto iy = 0u; iy < size.y; ++iy) {
				auto y = start + extent * (iy / float(size.y));
				for(auto ix = 0u; ix < size.x; ++ix) {
					auto x = start + extent * (ix / float(size.x));
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
			auto p = -0.5f * size + pos + Vec3f(off);
			cell.pos[i] = nytl::vec::cw::multiply(scale, p);
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
				++write;
			}
		}

		positions.resize(write);

		// smooth normals by area
		normals.resize(positions.size(), {0.f, 0.f, 0.f});
		for(auto i = 0u; i < indices.size(); i += 3) {
			auto e1 = positions[indices[i + 1]] - positions[indices[i + 0]];
			auto e2 = positions[indices[i + 2]] - positions[indices[i + 0]];

			auto normal = nytl::cross(e1, e2);
			normals[indices[i + 0]] += normal;
			normals[indices[i + 1]] += normal;
			normals[indices[i + 2]] += normal;
		}

		// normalize all
		for(auto& n : normals) {
			normalize(n);
		}
	}
};

class VolumeApp : public doi::App {
public:
	bool init(nytl::Span<const char*> args) override {
		if(!doi::App::init(args)) {
			return false;
		}

		auto& dev = vulkanDevice();
		auto bindings = {
			vpp::descriptorBinding(vk::DescriptorType::uniformBuffer,
				vk::ShaderStageBits::vertex | vk::ShaderStageBits::fragment)
		};

		dsLayout_ = {dev, bindings};
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

		auto _s1 = vpp::fillStaging(cb, positions_, doi::bytes(volume.positions));
		auto _s2 = vpp::fillStaging(cb, normals_, doi::bytes(volume.normals));
		auto _s3 = vpp::fillStaging(cb, indices_, doi::bytes(volume.indices));

		vk::endCommandBuffer(cb);
		qs.wait(qs.add(cb));

		return true;
	}

	void render(vk::CommandBuffer cb) override {
		vk::cmdBindPipeline(cb, vk::PipelineBindPoint::graphics, pipe_);
		doi::cmdBindGraphicsDescriptors(cb, pipeLayout_, 0, {ds_});
		vk::cmdBindVertexBuffers(cb, 0,
			{{positions_.buffer().vkHandle(), normals_.buffer().vkHandle()}},
			{{positions_.offset(), normals_.offset()}});
		vk::cmdBindIndexBuffer(cb, indices_.buffer(), indices_.offset(),
			vk::IndexType::uint32);
		vk::cmdDrawIndexed(cb, indexCount_, 1, 0, 0, 0);
	}

	void update(double dt) override {
		App::update(dt);

		auto kc = appContext().keyboardContext();
		if(kc) {
			doi::checkMovement(camera_, *kc, dt);
		}

		if(camera_.update) {
			App::scheduleRedraw();
		}
	}

	void updateDevice() override {
		App::updateDevice();

		if(camera_.update) {
			camera_.update = false;
			auto map = cameraUbo_.memoryMap();
			auto span = map.span();

			auto mat = matrix(camera_);
			doi::write(span, mat);
			doi::write(span, camera_.pos);
			map.flush();
		}
	}

	void resize(const ny::SizeEvent& ev) override {
		App::resize(ev);
		camera_.perspective.aspect = float(ev.size.x) / ev.size.y;
		camera_.update = true;
	}

	void mouseMove(const ny::MouseMoveEvent& ev) override {
		App::mouseMove(ev);
		if(rotateView_) {
			doi::rotateView(camera_, 0.005 * ev.delta.x, 0.005 * ev.delta.y);
			App::scheduleRedraw();
		}
	}

	bool mouseButton(const ny::MouseButtonEvent& ev) override {
		if(App::mouseButton(ev)) {
			return true;
		}

		if(ev.button == ny::MouseButton::left) {
			rotateView_ = ev.pressed;
			return true;
		}

		return false;
	}

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
	doi::Camera camera_;
	bool rotateView_ {};
};

int main(int argc, const char** argv) {
	VolumeApp app;
	if(!app.init({argv, argv + argc})) {
		return EXIT_FAILURE;
	}

	app.run();
}

