#pragma once

#include <tkn/types.hpp>
#include <tkn/file.hpp>
#include <nytl/span.hpp>
#include <type_traits>

// TODO: move to impl
#include <stb_image.h>
#include <dlg/dlg.hpp>
#include <cstdio>
#include <cerrno>

namespace tkn {

// Simple abstract stream interface.
// Mainly a way to abstract over data coming from a file or a memory
// buffer.
class Stream {
public:
	enum class SeekOrigin {
		set,
		curr,
		end,
	};

public:
	template<typename T>
	void read(T& val) {
		static_assert(std::is_trivially_copyable_v<T>);
		auto size = sizeof(val);
		read(size, reinterpret_cast<std::byte*>(&val));
	}

	template<typename T>
	T read() {
		T ret;
		read(ret);
		return ret;
	}

	virtual void read(u64 size, std::byte* buf) {
		auto res = readPartial(size, buf);
		if(res != i64(size)) {
			throw std::out_of_range("Stream::read");
		}
	}

	// Returns up to 'size' bytes into 'buf'.
	// When less bytes are available, reads the maximum amount available.
	// Always returns the number of bytes read, or a negative number
	// on error. Advances the current read address by the number of
	// read bytes.
	virtual i64 readPartial(u64 size, std::byte* buf) = 0;

	// Changes the current read address.
	// To be safe, the restriction from std::fseek apply.
	// They are quite complicated but the most important points:
	// - SeekOrigin::end is not supported on binary streams
	// - for text streams, offset must be zero or (for SeekOrigin::set),
	//   a value previously returned by tell.
	virtual void seek(i64 offset, SeekOrigin seek = SeekOrigin::set) = 0;

	// Returns the current absolute address in the stream.
	virtual u64 tell() const = 0;

	// Returns whether the stream is at the end.
	// Calling seek on it will clear this if appropriate.
	virtual bool eof() const = 0;
};

int streamStbiRead(void *user, char *data, int size) {
	auto stream = static_cast<Stream*>(user);
	return stream->readPartial(size, reinterpret_cast<std::byte*>(data));
}

void streamStbiSkip(void *user, int n) {
	auto stream = static_cast<Stream*>(user);
	stream->seek(n, Stream::SeekOrigin::curr);
}

int streamStbiEof(void *user) {
	auto stream = static_cast<Stream*>(user);
	return stream->eof();
}

const stbi_io_callbacks& streamStbiCallbacks() {
	static const stbi_io_callbacks impl = {
		streamStbiRead,
		streamStbiSkip,
		streamStbiEof,
	};
	return impl;
}

class FileStream : public Stream {
public:
	FileStream() = default;
	FileStream(File&& file) : file_(std::move(file)) {}

	static int origin(SeekOrigin origin) {
		switch(origin) {
			case SeekOrigin::set: return SEEK_SET;
			case SeekOrigin::curr: return SEEK_CUR;
			case SeekOrigin::end: return SEEK_END;
			default: throw std::logic_error("Invalid Stream::SeekOrigin");
		}
	}

	i64 readPartial(u64 size, std::byte* buf) override {
		// TODO: use count to prevent overflow?
		return std::fread(buf, size, 1u, file_);
	}

	void seek(i64 offset, SeekOrigin so) override {
		auto res = std::fseek(file_, offset, origin(so));
		if(res != 0) {
			dlg_error("fseek: {} ({})", res, std::strerror(errno));
			throw std::runtime_error("FileStream::fseek failed");
		}
	}

	u64 tell() const override {
		auto res = std::ftell(file_);
		if(res < 0) {
			dlg_error("ftell: {} ({})", res, std::strerror(errno));
			throw std::runtime_error("FileStream::ftell failed");
		}

		return u32(res);
	}

	bool eof() const override {
		return std::feof(file_);
	}

protected:
	File file_;
};

class MemoryStream : public Stream {
public:
	MemoryStream() = default;
	MemoryStream(nytl::Span<const std::byte*> buf);

	i64 readPartial(u64 size, std::byte* buf) override {
		auto read = std::clamp(i64(buf_.size()) - i64(at_), i64(0), i64(size));
		std::memcpy(buf, buf_.data() + at_, read);
		at_ += read;
		return read;
	}

	void seek(i64 offset, SeekOrigin origin) override {
		switch(origin) {
			case SeekOrigin::set: at_ = offset; break;
			case SeekOrigin::curr: at_ += offset; break;
			case SeekOrigin::end: at_ = buf_.size() + offset; break;
			default: throw std::logic_error("Invalid Stream::SeekOrigin");
		}
	}

	u64 tell() const override { return at_; }
	bool eof() const override { return at_ >= buf_.size(); }

protected:
	nytl::Span<const std::byte*> buf_;
	u64 at_ {0};
};

} // namespace tkn
