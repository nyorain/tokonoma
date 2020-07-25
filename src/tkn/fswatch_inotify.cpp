#include <tkn/fswatch.hpp>
#include <dlg/dlg.hpp>
#include <sys/inotify.h>
#include <unordered_set>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace tkn {

constexpr auto inotifyMask = IN_MODIFY | IN_MOVED_FROM;

struct FileWatcher::Impl {
	struct Entry {
		std::uint64_t id;
		std::string path;
	};

	int inotify;
	std::unordered_set<std::uint64_t> changed;
	std::unordered_map<unsigned, Entry> entries;
};

FileWatcher::FileWatcher() {
	impl_ = std::make_unique<Impl>();
	impl_->inotify = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if(impl_->inotify < 0) {
		dlg_error("Failed to init inotify: {} ({})", std::strerror(errno), errno);
		return;
	}
}

FileWatcher::~FileWatcher() = default;

// TODO: check if already existent, add dummy alias id.
std::uint64_t FileWatcher::watch(nytl::StringParam path) {
	if(!impl_ || !impl_->inotify) {
		dlg_warn("inotify not initialized");
		return 0u;
	}

	int wd = inotify_add_watch(impl_->inotify, path.c_str(), inotifyMask);
	if(wd < 0) {
		dlg_error("Failed to add inotify watcher for '{}': {} ({})",
			path, std::strerror(errno), errno);
		return 0u;
	}

	impl_->entries[wd] = {std::uint64_t(wd), std::string(path)};
	return wd;
}

void FileWatcher::unregsiter(std::uint64_t id) {
	if(!impl_ || !impl_->inotify) {
		return;
	}

	// TODO: this is really inefficient
	for(auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it) {
		if(it->second.id == id) {
			auto changedIt = impl_->changed.find(it->second.id);
			if(changedIt != impl_->changed.end()) {
				impl_->changed.erase(changedIt);
			}

			impl_->entries.erase(it);
			return;
		}
	}

	dlg_warn("Could not unregsiter FileSystemWatcher, invalid id");
}

void FileWatcher::update() {
	if(!impl_ || !impl_->inotify) {
		return;
	}

	char buffer[1024];
	ssize_t nr;
	size_t n;
	while((nr = read(impl_->inotify, buffer, sizeof(buffer))) > 0) {
		for(char* p = buffer; p < buffer + nr; p += n) {
			struct inotify_event* ev = (struct inotify_event*) p;
			n = sizeof(struct inotify_event) + ev->len;

			auto it = impl_->entries.find(ev->wd);
			if(it == impl_->entries.end()) {
				// This might happen when there were pending events
				// after it got destroyed. Just ignoring it should not
				// give any trouble.
				dlg_info("Receieved inotify event for unknown watchdog");
				continue;
			}

			auto& entry = it->second;
			impl_->changed.emplace(entry.id);

			// Some editors delete the file instead of just writing it.
			// In that case our watch got destroyed and we have to recreate it.
			// Also make sure to update our internal mapping.
			if(ev->mask & IN_IGNORED) {
				int wd = inotify_add_watch(impl_->inotify, entry.path.c_str(), inotifyMask);
				if(wd < 0) {
					dlg_error("Failed to (re-)add inotify watcher for '{}': {} ({})",
						ev->name, std::strerror(errno), errno);
					continue;
				}

				if(wd != int(entry.id)) {
					impl_->entries[wd] = std::move(entry);
					impl_->entries.erase(it);
				}
			}
		}
	}
}

bool FileWatcher::check(std::uint64_t id) {
	auto it = impl_->changed.find(id);
	if(it == impl_->changed.end()) {
		return false;
	}

	impl_->changed.erase(it);
	return true;
}

} // namespace tkn
