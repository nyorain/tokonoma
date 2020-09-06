#include <tkn/fswatch.hpp>
#include <dlg/dlg.hpp>

namespace tkn {

struct FileWatcher::Impl {
};

FileWatcher::FileWatcher() {
	impl_ = std::make_unique<Impl>();
}

FileWatcher::~FileWatcher() = default;

std::uint64_t FileWatcher::watch(nytl::StringParam path) {
    // TODO
    return 1u;
}

void FileWatcher::unregsiter(std::uint64_t id) {
    // TODO
}

void FileWatcher::update() {
    // TODO
}

bool FileWatcher::check(std::uint64_t id) {
    // TODO
    return false;
}

} // namespace tkn