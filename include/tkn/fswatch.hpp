#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <nytl/stringParam.hpp>

namespace tkn {

// TODO: callback-based interface.
// TODO: simple std::fs implementation for windows. Later on (or when
// neeeded for performance) this could be replaced by a intoify winapi
// equivalent.
// TODO: when on linux, we could add the inotify fd to the swa display
// event loop and just do nothing in update.

// Simple FileWatcher, able to notify on file change.
// Implementations may not be threadsafe.
class FileWatcher {
public:
	FileWatcher();
	~FileWatcher();

	std::uint64_t watch(nytl::StringParam path);
	void unregsiter(std::uint64_t);

	void update();
	bool check(std::uint64_t);

protected:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace tkn
