#include <asio/ip/udp.hpp>
#include <asio/ip/host_name.hpp>
#include <asio/io_service.hpp>
#include <asio/buffer.hpp>
#include <nytl/span.hpp>

#include <optional>
#include <functional>

using asio::ip::udp;
constexpr auto delay = 10u;
constexpr auto broadcastPort = 49163; // for initial broadcast


//XXX: stolen from kyo
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
	using MsgHandler = std::function<void(RecvBuf&)>;

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

private:
	void recvBroadcast(udp::endpoint& ep, std::uint32_t& num, unsigned& state);
	void recvSocket(udp::endpoint& ep, std::uint32_t& num, unsigned& state);

private:
	std::uint64_t step_ {1};
	std::uint64_t recv_ {0}; // last received
	// std::uint64_t ack_ {0}; // last of our messages acked by other side

	asio::io_service ioService_;
	std::optional<udp::socket> socket_;
	std::optional<udp::socket> broadcast_;

	std::vector<SendBuf> recvd_; // must be processed
	std::vector<SendBuf> ownPending_; // must be processed

	// std::vector<SendBuf> messages_; // old, for resending
	SendBuf sending_; // to be sent
};

enum class MessageType : std::uint32_t {
	build = 1,
	// field id (u32)
	// type (u32)

	velocity = 2,
	// field id (u32)
	// dir (vec2)
};
