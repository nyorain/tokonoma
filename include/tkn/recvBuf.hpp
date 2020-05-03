#pragma once

#include <cstddef>
#include <stdexcept>

namespace kyo {

// Represents an iterator over the raw non-owned data of a received message buffer.
// - current: The current position in the buffer.
// - end: The end of the buffer, points after the last valid byte
struct RecvBuf {
	const std::byte* current;
	const std::byte* end;
};

// This exception should be thrown if a processed message buffer is invalid in
// any way.
struct InvalidRecvBuf : public std::logic_error {
	using std::logic_error::logic_error;
};

// Thrown by next(MessageBuffer) when the requested access would exceed
// the received message buffers size.
// Deriving from BasicMessageBuffer is automatically assumes that the
// message buffer is invalid in this case.
struct OutOfRangeRecvBuf : public InvalidRecvBuf {
	using InvalidRecvBuf::InvalidRecvBuf;
};

// Interprets the data of the message buffer at the current position at the
// given type. Automatically advances the current pointer after this call.
// Throws an exception if there is not enough data left in the msg buffer.
// The buffer at current will simply be reinterpret_casted to const T&.
template<typename T>
const auto& read(RecvBuf& buf) {
	if(buf.current + sizeof(T) > buf.end) {
		throw OutOfRangeRecvBuf{"next(RecvBuf): would exceed size"};
	}

	auto pos = buf.current;
	buf.current += sizeof(T);
	return reinterpret_cast<const T&>(*pos);
}

} // namespace kyo
