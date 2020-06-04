#include <tkn/stream.hpp>

#include <dlg/dlg.hpp>
#include <cstdio>
#include <cerrno>


namespace tkn {
namespace {

int streamStbiRead(void *user, char *data, int size) {
	auto stream = static_cast<Stream*>(user);
	return stream->readPartial(reinterpret_cast<std::byte*>(data), size);
}

void streamStbiSkip(void *user, int n) {
	auto stream = static_cast<Stream*>(user);
	stream->seek(n, Stream::SeekOrigin::curr);
}

int streamStbiEof(void *user) {
	auto stream = static_cast<Stream*>(user);
	return stream->eof();
}

int cSeekOrigin(Stream::SeekOrigin origin) {
	switch(origin) {
		case Stream::SeekOrigin::set: return SEEK_SET;
		case Stream::SeekOrigin::curr: return SEEK_CUR;
		case Stream::SeekOrigin::end: return SEEK_END;
		default: throw std::logic_error("Invalid Stream::SeekOrigin");
	}
}

} // namespace tkn


const stbi_io_callbacks& streamStbiCallbacks() {
	static const stbi_io_callbacks impl = {
		streamStbiRead,
		streamStbiSkip,
		streamStbiEof,
	};
	return impl;
}

i64 FileStream::readPartial(std::byte* buf, u64 size) {
	// TODO: use count to prevent overflow?
	return std::fread(buf, 1u, size, file_);
}

void FileStream::seek(i64 offset, SeekOrigin so) {
	auto res = std::fseek(file_, offset, cSeekOrigin(so));
	if(res != 0) {
		dlg_error("fseek: {} ({})", res, std::strerror(errno));
		throw std::runtime_error("FileStream::fseek failed");
	}
}

u64 FileStream::address() const {
	auto res = std::ftell(file_);
	if(res < 0) {
		dlg_error("ftell: {} ({})", res, std::strerror(errno));
		throw std::runtime_error("FileStream::ftell failed");
	}

	return u32(res);
}

bool FileStream::eof() const {
	return std::feof(file_);
}

} // namespace tkn
