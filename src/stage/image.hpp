#pragma once

#include <cstddef>
#include <memory>
#include <nytl/stringParam.hpp>

// NOTE: we could direclty load the data onto a vk::Memory object
//   (or even directly into a buffer/image i guess we have to know
//   which one anyways for the layout).
// NOTE: optimal for deferred initialization: allow to retrieve a size
//   without the data, then later on get data (still with same open
//   "handle")

namespace doi {

struct Image {
	unsigned width;
	unsigned height;
	unsigned channels;
	std::unique_ptr<std::byte[]> data;
};

enum class Error {
	none,
	invalidPath,
	invalidType,
	internal,
};

Error loadJpeg(nytl::StringParam filename, Image&);
Error loadPng(nytl::StringParam filename, Image&);
Error load(nytl::StringParam filename, Image&);

} // namespace doi
