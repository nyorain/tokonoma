#pragma once

#include <tkn/types.hpp>
#include <tkn/file.hpp>
#include <tkn/bits.hpp>
#include <nytl/span.hpp>
#include <type_traits>
#include <stb_image.h>
#include <cstring>
#include <stdexcept>
#include <type_traits>

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
	virtual ~Stream() = default;

	// Returns up to 'size' bytes into 'buf'.
	// When less bytes are available, reads the maximum amount available.
	// Always returns the number of bytes read, or a negative number
	// on error. Advances the current read address by the number of
	// read bytes.
	virtual i64 readPartial(std::byte* buf, u64 size) = 0;

	// Changes the current read address.
	// To be safe, the restriction from std::fseek apply.
	// They are quite complicated but the most important points:
	// - SeekOrigin::end is not supported on binary streams
	// - for text streams, offset must be zero or (for SeekOrigin::set),
	//   a value previously returned by tell.
	virtual void seek(i64 offset, SeekOrigin seek = SeekOrigin::set) = 0;

	// Returns the current absolute address in the stream.
	virtual u64 address() const = 0;

	// Returns whether the stream is at the end.
	// Calling seek on it will clear this if appropriate.
	virtual bool eof() const = 0;

	// Utility
	// Like readPartial but throws when an error ocurrs or the
	// buffer can't be completely filled.
	virtual void read(std::byte* buf, u64 size) {
		auto res = readPartial(buf, size);
		if(res != i64(size)) {
			throw std::out_of_range("Stream::read");
		}
	}

	// Overloads that read into span-based buffers..
	virtual void read(nytl::Span<std::byte> buf) {
		read(buf.data(), buf.size());
	}

	virtual i64 readPartial(nytl::Span<std::byte> buf) {
		return readPartial(buf.data(), buf.size());
	}

	// Reads into the given object.
	// T must be standard layout type or vector of such.
	// Throws when the objects can't be filled completely or an error ocurrs.
	template<typename T>
	void read(T& val) {
		read(tkn::bytes(val));
	}

	// Tries to read the given object.
	// T must be standard layout type or vector of such.
	// Returns whether reading the complete object suceeded.
	template<typename T>
	bool readPartial(T& val) {
		auto bytes = tkn::bytes(val);
		return readPartial(bytes) == i64(bytes.size());
	}

	// Creates a (default-constructed) object of type T and attempts to
	// fill it from stream. T must be standard layout type or vector of such.
	// Throws when the objects can't be filled completely or an error ocurrs.
	template<typename T>
	T read() {
		T ret;
		read(ret);
		return ret;
	}
};

const stbi_io_callbacks& streamStbiCallbacks();

class FileStream : public Stream {
public:
	FileStream() = default;
	FileStream(File&& file) : file_(std::move(file)) {}

	i64 readPartial(std::byte* buf, u64 size) override;
	void seek(i64 offset, SeekOrigin so) override;
	u64 address() const override;
	bool eof() const override;

	std::FILE* file() const { return file_; }

protected:
	File file_;
};

class MemoryStream : public Stream {
public:
	MemoryStream() = default;
	MemoryStream(nytl::Span<const std::byte> buf) : buf_(buf) {}

	inline i64 readPartial(std::byte* buf, u64 size) override {
		auto read = std::clamp(i64(buf_.size()) - i64(at_), i64(0), i64(size));
		std::memcpy(buf, buf_.data() + at_, read);
		at_ += read;
		return read;
	}

	inline void seek(i64 offset, SeekOrigin origin) override {
		switch(origin) {
			case SeekOrigin::set: at_ = offset; break;
			case SeekOrigin::curr: at_ += offset; break;
			case SeekOrigin::end: at_ = buf_.size() + offset; break;
			default: throw std::logic_error("Invalid Stream::SeekOrigin");
		}
	}

	inline u64 address() const override { return at_; }
	inline bool eof() const override { return at_ >= buf_.size(); }

	inline nytl::Span<const std::byte> buffer() const { return buf_; }

protected:
	nytl::Span<const std::byte> buf_;
	u64 at_ {0};
};

} // namespace tkn
