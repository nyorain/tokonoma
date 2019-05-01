#pragma once

#include <cstddef>
#include <memory>
#include <nytl/stringParam.hpp>

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
