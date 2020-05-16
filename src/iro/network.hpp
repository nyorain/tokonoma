#pragma once

#include <tkn/types.hpp>
#include <nytl/span.hpp>

#include <asio/ip/udp.hpp>
#include <asio/ip/host_name.hpp>
#include <asio/io_service.hpp>
#include <asio/buffer.hpp>

#include <optional>
#include <functional>
#include <chrono>

using asio::ip::udp;
using namespace tkn::types;

constexpr auto packetMagic = 0xFB6180CA;
constexpr auto broadcastPort = 49163; // for initial broadcast

// Returns whether the given number is a power of two or zero
constexpr auto potOrZero(unsigned n) {
	return (n & (n - 1)) == 0;
}

// must be power of two, and smaller than 16 (so we can fit
// out ack bits in 32-bit numbers).
// Important that this is an unsigned number, otherwise step_ + delay
// might result in a signed number, messing with our wrapping logic.
constexpr u32 delay = 8u;
static_assert(delay > 0 && delay < 16 && potOrZero(delay));


/// Represents an owned data buffer to send.
/// Will allocate memory as needed. The "current" position is always the
/// end of the data array, i.e. there shall never be unused values in the
/// data array (just reserve size in it for performance).
struct SendBuf {
	std::vector<std::byte> data;
};

/// Writes the given value into the given send buffer.
/// Also returns a reference to the written value.
template<typename T>
auto& write(SendBuf& buf, T&& obj) {
	using TT = std::remove_cv_t<std::remove_reference_t<T>>;
	buf.data.resize(buf.data.size() + sizeof(T));
	return *reinterpret_cast<TT*>(buf.data.data() + buf.data.size() - sizeof(T)) = obj;
}

using RecvBuf = nytl::Span<const std::byte>;

// everything network related
class Socket {
public:
	using MsgHandler = std::function<void(std::uint32_t player, RecvBuf&)>;
	using Clock = std::chrono::steady_clock;

public:
	Socket();

	// sends pending messages and calls the message handler with the
	// messages to be processed. Returns whether allowed to make
	// the next step.
	bool update(MsgHandler handler);

	// write new messages into that buffer
	// until next nextStep call
	SendBuf& add() { return sending_; }

	udp::socket& socket() { return *socket_; }
	auto step() const { return step_; }
	auto player() const { return player_; }

private:
	void recvBroadcast(udp::endpoint& ep, std::uint32_t& num, unsigned& state);
	void recvSocket(udp::endpoint& ep, std::uint32_t& num, unsigned& state);

private:
	u32 step_ {0}; // current step (sent in next packets)
	u32 recv_ {0}; // bitset of received packets
	u32 ack_ {0}; // bitset of messages packets by other side (remoteAck)
	// acknowledge bitsets:
	// if bit i is set (ack & (1 << i)), this means that message
	// (step - delay + i) was received/acknowledged.
	// This means e.g. the bit at position 'delay' signals whether
	// the message for the same step for received.

	// counts the number of sent messages until we reach '2 * delay'
	// messages sent.
	u32 initCount_ {0};

	asio::io_service ioService_;
	std::optional<udp::socket> socket_;
	std::optional<udp::socket> broadcast_;

	// receieved messages to be processed in future
	std::vector<SendBuf> recvd_;

	// sent messages to be processed in future.
	// also used in case we have to re-send messages
	std::vector<SendBuf> sent_;

	SendBuf sending_; // to be sent, will be built in the current step
	unsigned player_; // own player id

	std::optional<Clock::time_point> waiting_;
};

enum class MessageType : std::uint32_t {
	build = 1,
	// field id (u32)
	// type (u32)

	velocity = 2,
	// field id (u32)
	// dir (vec2)
};
