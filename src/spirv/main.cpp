#include <tkn/headless.hpp>
#include <vpp/device.hpp>
#include <vpp/shader.hpp>
#include <vpp/submit.hpp>
#include <vpp/commandAllocator.hpp>
#include <vpp/pipeline.hpp>
#include <vpp/vk.hpp>
#include <vpp/util/file.hpp>
#include <dlg/dlg.hpp>

int main() {
	auto args = tkn::HeadlessArgs {};
	args.layers = true;

	auto headless = tkn::Headless(args);

	auto& dev = *headless.device;

	// create pipe
	vpp::PipelineLayout pipeLayout(dev, {}, {});

	auto spv = vpp::readFile("src/spirv/comp.spv", true);
	dlg_assert(spv.size() % 4 == 0);
	auto ptr = reinterpret_cast<const std::uint32_t*>(spv.data());
	long size = spv.size() / 4;

	vpp::ShaderModule shader(dev, {ptr, size});
	vk::ComputePipelineCreateInfo cpi;
	cpi.layout = pipeLayout;
	cpi.stage.module = shader;
	cpi.stage.stage = vk::ShaderStageBits::compute;
	cpi.stage.pName = "mymain";

	auto pipe = vpp::Pipeline(dev, cpi);

	// execute pipe
	auto& qs = dev.queueSubmitter();
	auto cb = dev.commandAllocator().get(qs.queue().family());

	vk::beginCommandBuffer(cb, {});
	vk::cmdBindPipeline(cb, vk::PipelineBindPoint::compute, pipe);
	vk::cmdDispatch(cb, 256, 1, 1);
	vk::endCommandBuffer(cb);

	qs.wait(qs.add(cb));
}
