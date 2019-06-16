#include "bugged.hpp"
#include <deferred/graph.hpp>

TEST(basic) {
	FrameGraph graph;
	auto& pass1 = graph.addPass();
	vk::Image img1;
	vk::Image img2;
	auto& out1 = pass1.addOut(syncScopeFlex, img1);
	auto& out2 = pass1.addOut(syncScopeFlex, img2);

	auto scope2 = SyncScope {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
	auto& pass2 = graph.addPass();
	pass2.addIn(out1, scope2);

	auto scope3 = SyncScope {
		vk::PipelineStageBits::fragmentShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};
	auto& pass3 = graph.addPass();
	pass3.addIn(out1, scope3);
	pass3.addIn(out2, scope3);

	graph.compute();

	// test
	auto& order = graph.order();
	EXPECT(graph.check(), true);

	EXPECT(order.size(), 3u);
	EXPECT(order[0].pass, &pass1);
	EXPECT(order[1].pass, &pass2);
	EXPECT(order[2].pass, &pass3);

	EXPECT(order[0].barriers.size(), 0u);
	EXPECT(order[1].barriers.size(), 0u);
	EXPECT(order[2].barriers.size(), 0u);

	EXPECT(graph.check(), true);
}

TEST(outOfOrder) {
	FrameGraph graph;
	vk::Image img1;
	vk::Image img2;
	auto& pass1 = graph.addPass();
	auto& pass3 = graph.addPass();
	auto& pass2 = graph.addPass();

	auto scope2 = SyncScope {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite
	};
	auto scope3 = SyncScope {
		vk::PipelineStageBits::fragmentShader,
		vk::ImageLayout::shaderReadOnlyOptimal,
		vk::AccessBits::shaderRead,
	};

	auto& out1 = pass1.addOut(syncScopeFlex, img1);
	auto& out2 = pass1.addOut(syncScopeFlex, img2);
	pass3.addIn(out2, scope3);

	auto& out3 = pass2.addInOut(out1, scope2);
	pass3.addIn(out3, scope3);

	graph.compute();

	// test
	EXPECT(graph.check(), true);

	auto& order = graph.order();
	EXPECT(order.size(), 3u);
	EXPECT(order[0].pass, &pass1);
	EXPECT(order[1].pass, &pass2);
	EXPECT(order[2].pass, &pass3);

	EXPECT(order[0].barriers.size(), 0u);
	EXPECT(order[1].barriers.size(), 0u);

	EXPECT(order[2].barriers.size(), 1u);
	auto& b = order[2].barriers[0];
	EXPECT(b.src, scope2);
	EXPECT(b.dst, scope3);
	EXPECT(b.target, &out3);

	EXPECT(graph.check(), true);
}

TEST(cycle) {
	FrameGraph graph;
	vk::Image img1;
	auto& pass = graph.addPass();
	auto& out = pass.addOut(syncScopeFlex, img1);
	pass.addIn(out, syncScopeFlex);
	EXPECT(graph.check(), false);
}

TEST(cycle2) {
	FrameGraph graph;
	vk::Image img1;
	vk::Image img2;
	auto& pass1 = graph.addPass();
	auto& pass2 = graph.addPass();

	auto& out1 = pass1.addOut(syncScopeFlex, img1);
	pass2.addIn(out1, syncScopeFlex);
	auto& out2 = pass2.addOut(syncScopeFlex, img2);
	pass1.addIn(out2, syncScopeFlex);

	EXPECT(graph.check(), false);
}

TEST(badDep) {
	FrameGraph graph;
	vk::Image img1;
	auto& pass1 = graph.addPass();
	auto& out1 = pass1.addOut(syncScopeFlex, img1);

	auto scope2 = SyncScope {
		vk::PipelineStageBits::computeShader,
		vk::ImageLayout::general,
		vk::AccessBits::shaderRead | vk::AccessBits::shaderWrite
	};
	auto& pass2 = graph.addPass();
	auto& out2 = pass2.addInOut(out1, scope2);

	auto& pass3 = graph.addPass();
	pass3.addIn(out1, syncScopeFlex);
	pass3.addIn(out2, syncScopeFlex);

	EXPECT(graph.check(), false);
}
