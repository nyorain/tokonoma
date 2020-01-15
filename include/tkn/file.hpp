#pragma once

#include <nytl/stringParam.hpp>
#include <memory>
#include <cstdio>

namespace tkn {

/// RAII wrapper around std::FILE
class File {
public:
	File() = default;
	explicit File(std::FILE* file) : file_(file) {}
	explicit File(nytl::StringParam path, nytl::StringParam mode) :
		file_(std::fopen(path.c_str(), mode.c_str())) {}

	~File() noexcept {
		if(file_) std::fclose(file_);
	}

	File(File&& rhs) noexcept : file_(rhs.file_) { rhs.file_ = {}; }
	File& operator=(File&& rhs) noexcept {
		if(file_) std::fclose(file_);
		file_ = rhs.file_;
		rhs.file_ = {};
		return *this;
	}

	std::FILE* release() {
		auto ret = file_;
		file_ = {};
		return ret;
	}

	std::FILE* get() const { return file_; }
	operator std::FILE*() const { return file_; }
	operator bool() const { return file_; }

private:
	std::FILE* file_ {};
};

} // namespace tkn
