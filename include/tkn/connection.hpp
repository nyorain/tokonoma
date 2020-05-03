#pragma once

// NOTE: moved here from kyo.
// pretty sure it contains some issues.
// TODO: also move the tests here

#include <asio/buffer.hpp>
#include <nytl/span.hpp>

#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <bitset>
#include <chrono>
#include <functional>

namespace tkn {

struct RecvBuf;

/// Magic package numbers.
namespace magic {
	/// Magic numbers with which the package starts.
	/// HeaderMagic indicates that this is the first fragment of a package an includes
	/// a message header. FragmentMagic indicates that this is a later part of a
	/// fragment and only has a FragmentHeader.
	static constexpr auto message = 24375;
	static constexpr auto fragment = 55620;

	/// The magic numbers to end a package with
	/// EndMagic is used for the last fragment of a package while
	/// endAnotherMagic indicates that there is at least 1 next fragment.
	static constexpr auto end = 42523;
	static constexpr auto another = 01532;
}

/// Header of a sent message.
/// The ackBits value represent (bitwise) whether the last packages before ack (the
/// last seen package) has been received as well.
/// Example: The most significant bit in ackBits represents whether the package with
/// the id 'ack - 32' has been received from the other side.
struct MessageHeader {
	uint32_t magic = magic::message;
	uint32_t seq; // sequence number of this package
	uint32_t ack; // latest seen package from other end
	uint32_t ackBits; // last received packages, acknowledge bits (relative to ack)
};

/// Header a fragment package.
struct FragmentHeader {
	uint32_t magic = magic::fragment;
	uint32_t seq; // the sequence number of the related message
	uint32_t fragment; // the fragment number
};


/// Represents the various detected errors a MessageHeader can have from
/// the point of view of a ConnectionManager.
enum class MessageHeaderStatus {
	valid = 0,
	invalidMagic,
	seqDiff,
	alreadyReceived,
	ackDiff,
	ackOld,
	ackNew
};

/// Returns the name of a MessageHeaderStatus value.
const char* name(MessageHeaderStatus mhs);

/// Manages sequence numbers and acknowledgements of sequences
/// for an udp connection. Used to build and process MessageHeaders.
/// For correct behaviour, all messages send to the other end must include
/// a header retrieved using the 'nextHeader' functions and the headers
/// of all retrieved messages must be passed to 'processHeader'.
class ConnectionManager {
public:
	/// The number of acknowledged packages to store for tracking.
	/// Note that if the gap between the last acknowledged sequence numbers
	/// in two succeeding packages is larger than the stored ackBits in
	/// the message header, all package sequence numbers we don't have any ack information
	/// about will be treated as lost (since in this case something went wrong, anyways).
	static constexpr auto remoteAckStoreCount = 1024;

	/// Setting for the rather heuristic sequence number validation
	/// the maximum seq number difference accepted
	/// packages in the range [remoteSeq_ - maxSeqDiff, remoteSeq_ + maxSeqDiff] are
	/// accepted. Also used to validate acknowledgements and other sequence number
	/// related checks. If anywhere is a jump of more than this count, the connection
	/// is basically broken.
	/// XXX any way to recover from it? any way to not require a hard connection reset?
	static constexpr auto maxSeqDiff = 1024;
	using Clock = std::chrono::steady_clock;

	/// The number of ping times stores.
	/// Should not exceed a reasonable amount, this many pings are stored as array.
	static constexpr auto pingStoreCount = 5;

public:
	/// Generates the message header for the next message.
	/// Increases the local sequence number.
	MessageHeader nextHeader();

	/// Processes an received message header.
	/// Returns its status, i.e. if it was valid or its first detected defect.
	/// If it was invalid, not changes to local state will be made.
	/// Headers are invalid if they have an invalid magic number or are too
	/// old.
	MessageHeaderStatus processHeader(const MessageHeader& msg);

	/// Returns the last used local sequence number.
	auto localSeq() const { return localSeq_; }

	/// Returns the highest received sequence number from the other side.
	/// This indicates the last sequence id we acknowledge when sending message
	/// to the other side.
	auto remoteSeq() const { return remoteSeq_; }

	/// Returns a bit mask indicating which of the last packages sent from remote
	/// were received on this end.
	auto localAckBits() const { return localAckBits_; }

	/// Returns the highest local sequence number acknowledged by the other side.
	auto remoteAck() const { return remoteAck_; }

	/// Returns whether the package with the given sequence number was acknowledged
	/// by the other side. Note that this returns false if the given
	/// sequence id is no longer cached, i.e. ackStoreCount behind the
	/// last acknowledges sequence number of the other side.
	bool acknowledged(uint32_t sequenceNumber) const;

	/// Returns the last 'pingStoreCount' ping time (in microseconds, measured by Clock).
	/// The ping times (naturally) always include the process delay, i.e.
	/// the time needed between receiving a package and sending the ack package (so
	/// it is NO fully correct network-ping time).
	/// If a pinged package is lost, the ping time will include the time
	/// until a newer package is acknowledged.
	decltype(auto) pings() const { return pings_; }

protected:
	std::uint32_t localSeq_ = 0; // seq number of the last message sent
	std::uint32_t remoteSeq_ = 0; // highest seq number of a received message, ack
	std::uint32_t localAckBits_ = 0; // which last remote messages were retrieved on this end

	std::uint32_t remoteAck_ = 0; // last package that was acknowledged by the other side

	// the last sent ping-relevant message and its time
	std::array<unsigned int, pingStoreCount> pings_ = {};
	std::uint32_t pingSent_ = 0;
	Clock::time_point pingTime_ = {};

	// which package the other side acknowledged
	// remoteAckBits_[i] represents whether package with sequence number
	// remoteAck_ - i - 1  was acknowledged by the other side
	std::bitset<remoteAckStoreCount> remoteAckBits_ = {};
};



/// The status of a MessageManager for a received package.
enum class PackageStatus {
	invalid, // package was invalid in any way
	invalidMessage, // package was valid but completed invalid package message data
	fragment, // (so-far) valid fragment of a not-yet finished fragmented package
	message // package finished a complete fragmented package, was fully valid
};

/// Derivation from ConnectionManager that handles messages to send.
/// Implements the concept of separating critical messages (that have to reach the other
/// side are sent again every package until acknowledged) and non-critical messages (that
/// are only sent once but whose arrival can still be tracked).
/// Note that ConnectionManager is not a virtual class, so an object of this type
/// should be used (and especially not destrued) as a ConnectionManager.
/// It just builds upon its functionality.
class MessageManager : public ConnectionManager {
public:
	/// The maximum size of packages in bytes (only the raw buffers).
	/// Used to avoid fragmentation or higher package lost rates.
	static constexpr auto maxPackageSize = 1200;

	/// The function responsible for handling received messages.
	/// See the messageHandler function for more information.
	/// \param seq The sequence number this message belongs to
	/// \param buf The packages message buffer pointing to the start of a message.
	/// Must be advanced to the end of the message.
	using MessageHandler = std::function<bool(uint32_t seq, RecvBuf& buf)>;

	/// A critical message. Contains data and associated sequence number (i.e. the
	/// sequence number it was first sent with).
	struct Message {
		std::vector<std::byte> data;
		uint32_t seq;
	};

public:
	// --- Sending ---
	/// Queues the given non-critical message up for the next frame.
	/// Returns the sequence number of the next message.
	/// The sequence number can be used to check whether the other side
	/// acknowledged the message, by calling acknowledged(sequence number).
	/// Note that due to the non-critical nature of the message it might
	/// never be received. After some time (after ackStoreCount new sent packages),
	/// the sequence id will no longer be tracked.
	uint32_t queueMsg(nytl::Span<const std::byte> msg);

	/// Queues the given critical message.
	/// Returns the sequence number of the next message.
	/// The sequence number can be used to check whether the other side has
	/// acknowledged the message by checking if remoteAck() is larger
	/// than the returned sequence number. Using this approach, one has to
	/// care about sequence number wrapping.
	uint32_t queueCriticalMsg(nytl::Span<const std::byte> msg);

	/// Prepares and returns the next packages to be sent.
	/// The returned buffers will remain valid until the next time this function is called.
	/// Will remove all acknowledged critical messages before preparing and
	/// all queued nonCritical messages after preparing.
	const std::vector<asio::const_buffer>& packages();

	/// Returns all queued critical messages.
	/// \param update If this is true, this calls updates the internal vector, i.e.
	/// clears packages that were acknowledged.
	/// Note that there may actually be messages left in the queue that
	/// were already acknowledged if it is not updated.
	const std::vector<Message>& criticalMessages(bool update = false);
	const std::vector<Message>& criticalMessages() const { return critical_; }

	/// Returns the currently queued non-critical messages.
	decltype(auto) nonCriticalMessage() const  { return nonCritical_; }

	/// Clears all pending critical messages.
	/// Returns the moved vector.
	std::vector<Message> clearCriticalMessages() { return std::move(critical_); }




	// --- Receiving ---
	/// Processes the given received package.
	/// Returns the status of the message.
	/// If it returns PackageStatus::message this package caused
	/// a new package (and therefore new messages) to be available and has already
	/// called the message handler with success.
	/// If it returns PackageStatus::invalid the buffer itself could not be parsed.
	/// If it returns PackageStatus::invalidMessage the buffer completed
	/// a fragmented package (or was self-contained) but could not be correctly
	/// parsed by the messgae handler.
	/// If it returns PackageStatus::fragment the package was a valid fragment that
	/// did not complete a package.
	PackageStatus processPackage(asio::const_buffer buffer);

	/// Sets the callback for messages to be processed.
	/// This will only be called from within processPackage.
	/// The handler will receive a MessageBuffer that points to the
	/// beginning of a message. It must advance it behind the message (to the
	/// first byte after the end of the message).
	/// If the handler throws MsgBufInvalid or returns false or if there is an internal
	/// error while processing the package, the whole package is treated
	/// as invalid and will not be further processed.
	/// Errors other than MsgBufInvalid form the message handler are just propagated
	/// out of the processPackage function.
	/// Might be set to an empty handler in which case no messages are processed
	/// and processPackage will always trigger invalid message return values.
	/// Returns the moved old handler. At the beginning is initialized with an no
	/// handler.
	/// This function must not be called from inside the old message handler.
	MessageHandler messageHandler(MessageHandler newHandler);

	/// Frees all stored fragments that are older than the given time.
	/// Returns the number of discarded fragmented packages.
	unsigned int discardFragments(Clock::duration age);

	/// Frees all currently unused memory.
	void shrink();

protected:
	/// Handles the given package data, i.e. dispatches messages to the message handler.
	/// The passed ConstMsgBuf points to the beginning of the package data and its
	/// size only contains the size of the package data (excluding end magic value).
	/// Returns whether the buffer could completely handled.
	/// \param lastAck The previously last acknowledged package.
	/// When this function is calld the header of this package was already processed
	/// so this information is needed
	/// \param seqNumber the sequence number of the package
	bool handlePackageData(uint32_t lastAck, uint32_t seqNumber, RecvBuf);

	/// Clears critical messages that were acknowledged by the other side from
	/// critical_
	void updateCriticalMessages();

protected:
	std::vector<Message> critical_; // stores all critical messages, sorted
	std::vector<std::vector<std::byte>> nonCritical_; // stores all non-critical pending messages
	std::vector<std::byte> packageBuffer_; // raw package buffer store
	std::vector<asio::const_buffer> buffers_; // buffers sent in last step

	MessageHandler messageHandler_ {}; // current message handler, might be empty

	/// Contains some currently unused buffers that will be reused the next time
	/// a buffer is needed. Separated since msg buffers are usually way smaller
	/// than pkg buffers. They may still contain data, must be cleared when popped
	std::vector<std::vector<std::byte>> unusedMsgBuffers_;
	std::vector<std::vector<std::byte>> unusedPkgBuffers_; // used for fragmented pkgs

	/// A fragmented package that is currently being assembled.
	/// Contains the packages sequence number as well as all fragments.
	/// If a vector in fragments is empty the associated fragment has not yet
	/// been received and must be waited on.
	/// Fragmented pacakges are discarded after some time if not all fragments
	/// have arrived.
	struct FragmentedPackage {
		Clock::time_point firstSeen; // first encounter of any fragment
		MessageHeader header;
		std::vector<bool> received; // which fragments where received
		std::vector<std::byte> data; // the raw package data (stripped headers and magic)
	};

	std::vector<FragmentedPackage> fragmented_; // sorted
};

} // namespace tkn

