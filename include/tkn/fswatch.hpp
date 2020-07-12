#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <nytl/stringParam.hpp>

namespace tkn {

// Simple FileWatcher, able to notify on file change.
// Implementations may not be threadsafe.
class FileWatcher {
public:
	FileWatcher();
	~FileWatcher();

	std::uint64_t registerPath(nytl::StringParam path);
	void unregsiter(std::uint64_t);

	void update();
	bool check(std::uint64_t);

protected:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace tkn
