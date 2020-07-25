#pragma once

#include <tkn/render.hpp>
#include <vpp/image.hpp>
#include <vpp/vk.hpp>

// WIP high level texture concept and utility.

namespace tkn {

class Texture {
private:
	vpp::ViewableImage image_;

	// only store relevant parts?
	// for what exactly are they relevant though?
	// we don't ever really need this, this does not solve anything.
	vk::ImageCreateInfo imgCreateInfo_;
	vk::ImageViewCreateInfo viewCreateInfo_;
};

struct ImageUser {
	vk::Image image;
	vk::CommandBuffer cb;
	SyncScope srcScope;
};

void transition(ImageUser& imageUser, SyncScope dst);

class BarrierBuilder {
public:
	void imageBarrier(vk::Image, SyncScope src, SyncScope dst);
	void imageBarrier(ImageUser& user, SyncScope dst);

	void bufferBarrier(const vpp::BufferSpan&, SyncScope src, SyncScope dst);
	void memoryBarrier(SyncScope src, SyncScope dst);
	void fullDebugBarrier();
	void record(vk::CommandBuffer cb);

protected:
	vk::PipelineStageBits srcStages_;
	vk::PipelineStageBits dstStages_;
};

} // namespace tkn
