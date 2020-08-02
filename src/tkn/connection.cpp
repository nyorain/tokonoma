#undef DLG_DEFAULT_TAGS
#define DLG_DEFAULT_TAGS "tkn", "network"

#include <tkn/connection.hpp>
#include <tkn/recvBuf.hpp>
#include <dlg/dlg.hpp>
#include <limits>
#include <cmath>
#include <algorithm>
#include <iostream>

// NOTE: do we need ackbits at all? it currently is only used for validation (?)

namespace tkn {

// utility
const char* name(MessageHeaderStatus mhs) {
	switch(mhs) {
		case MessageHeaderStatus::valid: return "valid";
		case MessageHeaderStatus::invalidMagic: return "invalidMagic";
		case MessageHeaderStatus::ackDiff: return "ackDiff";
		case MessageHeaderStatus::seqDiff: return "seqDiff";
		case MessageHeaderStatus::ackOld: return "ackOld";
		case MessageHeaderStatus::ackNew: return "ackNew";
		case MessageHeaderStatus::alreadyReceived: return "alreadyReceived";
		default: return "<unknown>";
	}
}
// ConnectionManager
MessageHeader ConnectionManager::nextHeader()
{
	MessageHeader ret {};
	ret.seq = ++localSeq_;
	ret.ack = remoteSeq_;
	ret.ackBits = localAckBits_;

	// send a new ping if there is no active one
	if(remoteAck_ - pingSent_ < maxSeqDiff) {
		pingSent_ = localSeq_;
		pingTime_ = Clock::now();
	}

	return ret;
}

MessageHeaderStatus ConnectionManager::processHeader(const MessageHeader& msg)
{
	dlg_tags("ConnectionManager", "processHeader");

	using MHS = MessageHeaderStatus;

	// first make sure the header is valid, then change local state
	// check magic number
	if(msg.magic != magic::message) {
		dlg_info("message header has invalid magic number");
		return MHS::invalidMagic;
	}

	// - remote sequence number ---------------------------------------
	// the absolute difference between seq numbers across wrap boundary
	// if there is a too large gap, the package is treated as invalid.
	auto absSeqDiff = std::min(msg.seq - remoteSeq_, remoteSeq_ - msg.seq);
	if(absSeqDiff > maxSeqDiff) {
		dlg_info("seqDiff too high, msg.seq: {}, remoteSeq: {}", msg.seq, remoteSeq_);
		return MHS::seqDiff;
	}

	// check if this is the same package as the one referenced by remoteSeq_
	if(absSeqDiff == 0) {
		dlg_info("seq {} already received(1)", msg.seq);
		return MHS::alreadyReceived;
	}

	// whether to update the remote sequence number
	// if this is true this is the newest pacakge ever received from the other side
	auto newRemoteSeq = (msg.seq - remoteSeq_) <= maxSeqDiff;

	// if it is and old message, it is invalid if it was already received (tracked
	// by localAckBits_)
	if(!newRemoteSeq && absSeqDiff < 32 && (localAckBits_ & (1 << (absSeqDiff - 1)))) {
		dlg_info("seq {} already received(2)", msg.seq);
		return MHS::alreadyReceived;
	}

	// - remote acknowledged seq -----------------------------------------
	// package acknowledged is not one of the last maxSeqDiff sent packages
	if(localSeq_ - msg.ack > maxSeqDiff) {
		dlg_info("ackDiff too high, msg.ack: {}, localSeq: {}", msg.ack, localSeq_);
		return MHS::ackDiff;
	}

	// newest package AND package acknowledged is older than the last acknowledged package
	if(newRemoteSeq && msg.ack - remoteAck_ > maxSeqDiff) {
		dlg_info("newer package with older ack {}, remoteAck_: {}", msg.ack, remoteAck_);
		return MHS::ackOld;
	}

	// older package AND acknowledged package is newer than the last acknowledged package
	if(!newRemoteSeq && remoteAck_ - msg.ack > maxSeqDiff) {
		dlg_info("older package with newer ack {}, remoteAck_: {}", msg.ack, remoteAck_);
		return MHS::ackNew;
	}

	// - ping check --------------------------------------------------
	if(remoteAck_ - pingSent_ > maxSeqDiff && msg.ack - pingSent_ < maxSeqDiff) {
		for(auto i = 0u; i < pingStoreCount - 1; ++i)
			pings_[i] = pings_[i + 1];
		auto diff = Clock::now() - pingTime_;
		auto count = std::chrono::duration_cast<std::chrono::microseconds>(diff).count();
		dlg_debug("recieved pong after {} microseconds", count);
		pings_.back() = count;
	}

	// - update local state -------------------------------------
	// we know the package is valid and apply its information
	if(newRemoteSeq) { // i.e. new message
		// ackBits are alwasy relative to the last seen package so we have to shift it
		// shift to the left since the most significant bit is the oldest one whose
		// bit is no longer needed
		localAckBits_ <<= absSeqDiff;
		remoteSeq_ = msg.seq;

		// make sure to set the localAck bit for the old latest ack (if possible)
		if(absSeqDiff < 32)
			localAckBits_ |= (1 << (absSeqDiff - 1));

		auto remoteAckDiff = msg.ack - remoteAck_;
		remoteAckBits_ <<= remoteAckDiff;
		remoteAck_ = msg.ack;

		// make sure to set the remoteAck bit for the old latest ack (if possible)
		if(remoteAckDiff < remoteAckBits_.size())
			remoteAckBits_ |= (1 << (remoteAckDiff - 1));
	} else { // i.e. old message
		// update localAckBits if in range
		localAckBits_ |= 1 << (absSeqDiff - 1);

		// TODO: update remoteAckBits_ here as well?
		// we might get new information from this old package
	}

	return MHS::valid;
}

bool ConnectionManager::acknowledged(uint32_t sequenceNumber) const
{
	auto diff = remoteAck_ - sequenceNumber;
	if(diff == 0) return true; // last acknowledged package
	if(diff > remoteAckStoreCount) return false; // out of date
	return remoteAckBits_[diff - 1];
}

// MessageManager
uint32_t MessageManager::queueMsg(nytl::Span<const std::byte> msg)
{
	dlg_assertlm(dlg_level_debug, !msg.empty(), "empty message queued");

	// try to find a matching unused msg buffer
	// XXX: we could iterate over the vector here and try to find a one
	// with a matching size, +memory -performance
	std::vector<std::byte> buffer {};
	if(!unusedMsgBuffers_.empty()) {
		buffer = std::move(unusedMsgBuffers_.back());
		unusedMsgBuffers_.pop_back();
	}

	buffer.clear();
	buffer.insert(buffer.end(), msg.begin(), msg.end());

	nonCritical_.emplace_back(std::move(buffer));
	return localSeq_ + 1;
}

uint32_t MessageManager::queueCriticalMsg(nytl::Span<const std::byte> msg)
{
	dlg_assertlm(dlg_level_debug, !msg.empty(), "empty message queued");

	// try to find a matching unused msg buffer
	// XXX: we could iterate over the vector here and try to find a one
	// with a matching size, +memory -performance
	Message cmsg {};
	if(!unusedMsgBuffers_.empty()) {
		cmsg.data = std::move(unusedMsgBuffers_.back());
		unusedMsgBuffers_.pop_back();
	}

	cmsg.data.clear();
	cmsg.data.insert(cmsg.data.end(), msg.begin(), msg.end());

	// insert the message just at the end
	// this automatically assures that the vector will be sorted.
	// use localSeq_ + 1 since this critical message will be first sent with
	// the next package
	cmsg.seq = localSeq_ + 1;
	critical_.emplace_back(std::move(cmsg));
	return localSeq_ + 1;
}

//  - utility -
namespace {
/// Returns a casted reference of 'T' from the given pointer and increases
/// the pointer sizeof(T)
template<typename T>
decltype(auto) nextCast(std::byte*& ptr)
{
	auto pos = ptr;
	ptr += sizeof(T);
	return reinterpret_cast<T&>(*pos);
}

} // anonymous namespace

const std::vector<asio::const_buffer>& MessageManager::packages()
{
	updateCriticalMessages();

	// clear previous buffers buffer
	buffers_.clear();
	packageBuffer_.clear();
	packageBuffer_.resize(maxPackageSize);

	// ptr: points to the first unwritten bytes in packageBuffer_
	// fragBegin: points to the begin of the current message
	// fragEnd: points to the address in which the end magic value will be written.
	auto ptr = packageBuffer_.data();
	auto fragBegin = packageBuffer_.data();
	auto fragEnd = fragBegin + maxPackageSize - 4;
	nextCast<MessageHeader>(ptr) = nextHeader();

	// the current fragment part
	// note that the first fragment header has part 1
	auto fragPart = 0u;

	// if there are no messages to send just return the single package filled
	// with the message header. Might be useful for acknowleding packages without
	// sending additional data
	if(critical_.empty() && nonCritical_.empty()) {
		nextCast<uint32_t>(ptr) = magic::end; // signal message end
		buffers_.push_back(asio::buffer(packageBuffer_.data(), ptr - fragBegin));
		return buffers_;
	}

	// this buffer will hold the indices of packageBuffer_ on which a new fragment starts
	// excludes the first fragments start value (0)
	// we cannot directly insert into buffers_ since the data in packageBuffers_
	// might be reallocated multiple times during this function
	std::vector<unsigned int> fragments {};

	// initialize the associated sequence number to a value different
	// than the first critical package so it will write it
	auto currentSeq = localSeq_ + 1;
	if(!critical_.empty())
		currentSeq = critical_.front().seq + 1;

	// function that writes the given data into the message buffer
	// makes sure that there is enough space in the current fragment
	auto write = [&](const auto* data, auto size) {
		auto remaining = size;
		while(remaining != 0) {
			// create new fragment if we reached its end
			// write magic end value
			// we always assume that other con
			if(ptr == fragEnd) {
				// end fragment
				nextCast<uint32_t>(ptr) = magic::another;
				auto oldSize = packageBuffer_.size();

				// push message into buffers
				fragments.push_back(oldSize);

				// create next message
				packageBuffer_.resize(oldSize + maxPackageSize);
				fragBegin = ptr = &packageBuffer_[oldSize];
				fragEnd = fragBegin + maxPackageSize - 4;

				// insert next fragment header
				nextCast<FragmentHeader>(ptr) = {magic::fragment, localSeq(), ++fragPart};
			}

			// write as much as possible
			// keep spacing for potential next seq number
			// the spacing is only needed in the last fragment we touch
			auto size = std::min<unsigned int>(remaining, fragEnd - ptr);

			std::memcpy(ptr, data, size);
			remaining -= size;
			ptr += size;
		}
	};

	// function that determines the byte count for the message group
	// with the given sequence number from the given iterator in
	// critical. Will also add the size of nonCritical messages
	// if they belong into this message group
	auto messageGroupSize = [&](uint32_t seq, auto it) {
		uint32_t size = 0u;

		// iterate over left critical messages
		while(it != critical_.end() && it->seq == seq) {
			size += it->data.size();
			++it;
		}

		// if all critical messages were included and the sequence
		// number matches also add non critical message sizes
		// TODO: it == critical_.end() really needed here
		if(it == critical_.end() && seq == localSeq_) {
			for(auto& msg : nonCritical_)
				size += msg.size();
		}

		return size;
	};

	// - write all critical messages -
	for(auto it = critical_.begin(); it != critical_.end(); ++it) {
		auto& msg = *it;

		// check if a new message group has to be started
		if(msg.seq != currentSeq) {
			currentSeq = msg.seq;
			auto size = messageGroupSize(msg.seq, it);
			write(&msg.seq, sizeof(msg.seq));
			write(&size, sizeof(size));
		}

		// write current critical message
		write(msg.data.data(), msg.data.size());
	}

	// - wirte all non-critical messages -
	if(!nonCritical_.empty()) {
		// check if the current package group already has the right
		// sequence number of if we have to start a new one
		// all non-critical messages belong to the localSeq_ seq number
		if(currentSeq != localSeq_) {
			currentSeq = localSeq_;
			auto size = messageGroupSize(localSeq_, critical_.end());
			write(&localSeq_, sizeof(localSeq_));
			write(&size, sizeof(size));
		}

		// write all non-critical messages
		// we dont need any spacing anymore since there won't be any sequence indicators
		// after this
		for(auto& msg : nonCritical_)
			write(msg.data(), msg.size());
	}

	// now push the fragments into buffers_
	auto prevFrag = 0u;
	auto totalSize = 0u;
	for(auto frag : fragments) {
		buffers_.push_back(asio::buffer(&packageBuffer_[prevFrag], frag - prevFrag));
		totalSize += frag - prevFrag;
		prevFrag = frag;
	}

	// End the last fragment and push it into the buffers
	// if the last fragment has no data abandon it and write the magic::end to the
	// last previous fragment. Note that if the last fragment was the first
	// fragment it had a message header (with larger size) and therefore will
	// always be added.
	if(static_cast<unsigned int>(ptr - fragBegin) == sizeof(FragmentHeader)) {
		*reinterpret_cast<uint32_t*>(fragBegin - 4) = magic::end;
	} else {
		nextCast<uint32_t>(ptr) = magic::end;
		buffers_.push_back(asio::buffer(&packageBuffer_[prevFrag], ptr - fragBegin));
		totalSize += ptr - fragBegin;
	}

	// clear non-critical messages
	unusedMsgBuffers_.insert(
		unusedMsgBuffers_.end(),
		std::make_move_iterator(nonCritical_.begin()),
		std::make_move_iterator(nonCritical_.end()));
	nonCritical_.clear();

	dlg_debug("{} packages to send, total size {} ",
		buffers_.size(), totalSize);

	return buffers_;
}

const std::vector<MessageManager::Message>& MessageManager::criticalMessages(bool update)
{
	if(update) updateCriticalMessages();
	return critical_;
}

void MessageManager::updateCriticalMessages()
{
	// TODO: really use eaxSeqDiff here?
	auto it = critical_.begin();
	for(; it != critical_.end() && remoteAck_ - it->seq <= maxSeqDiff; ++it);

	if(it == critical_.begin())
		return;

	// clear critical_, move buffers back to unusedMsgBuffers_
	unusedMsgBuffers_.reserve(unusedMsgBuffers_.size() + it - critical_.begin());
	for(auto i = critical_.begin(); i != it; ++i)
		unusedMsgBuffers_.push_back(std::move(i->data));
	critical_.erase(critical_.begin(), it);
}

PackageStatus MessageManager::processPackage(asio::const_buffer buffer)
{
	dlg_tags("MessageManager", "processPackage");

	// utility
	// the max raw package data size of the first fragment
	constexpr auto firstDataSize = maxPackageSize - sizeof(MessageHeader) - 4;

	// the max raw package data size of a non-first fragment
	constexpr auto fragDataSize = maxPackageSize - sizeof(FragmentHeader) - 4;

	// check that it at least has the size of the smaller header and end magic
	// for all first checks we only outputs info messages for invalid packages
	// since they might simply come from something else and are not really
	// an issue
	auto size = asio::buffer_size(buffer);
	if(size < sizeof(FragmentHeader) + 4) {
		dlg_info("invalid pkg: size {} too small", size);
		return PackageStatus::invalid;
	}

	// check magic numbers
	auto data = asio::buffer_cast<const std::byte*>(buffer);
	auto beginMagic = *reinterpret_cast<const uint32_t*>(data);
	auto endMagic = *reinterpret_cast<const uint32_t*>(data + size - 4);

	if(endMagic != magic::end && endMagic != magic::another) {
		dlg_info("invalid pkg: invalid end magic value {}", endMagic);
		return PackageStatus::invalid;
	}

	// fragment handling variables
	uint32_t seqid = 0u; // the sequence id the fragment belongs to (if it is an fragment)
	uint32_t fragpart = 0u; // the part the fragment has
	const MessageHeader* header = nullptr; // potential message header
	const std::byte* dataBegin = nullptr; // raw data begin
	const std::byte* dataEnd = (data + size) - 4; // raw data end

	// check message header or fragment header
	if(beginMagic == magic::message) {
		header = reinterpret_cast<const MessageHeader*>(data);
		if(endMagic == magic::end) {
			// we received a single, non-fragmented message, yeay
			// handle its header an pass it to handlePackageData
			auto lastAck = remoteSeq_;
			auto processed = processHeader(*header);
			if(processed != MessageHeaderStatus::valid) {
				dlg_info("invalid pkg: processing sc message header failed: {}", name(processed));
				return PackageStatus::invalid;
			}

			RecvBuf msgbuf {};
			msgbuf.current = data + sizeof(MessageHeader);
			msgbuf.end = data + size - 4; // exclude last magic

			return handlePackageData(lastAck, header->seq, msgbuf) ?
				PackageStatus::message :
				PackageStatus::invalidMessage;
		}

		// if it was only the first part of the fragmented message we wait with processing
		// the header until all fragments part arrive (if they do)
		seqid = header->seq;
		fragpart = 0u; // first fragment
		dataBegin = data + sizeof(MessageHeader);
	} else if(beginMagic == magic::fragment) {
		auto& header = *reinterpret_cast<const FragmentHeader*>(data);
		seqid = header.seq;
		fragpart = header.fragment;
		dataBegin = data + sizeof(FragmentHeader);
	} else {
		dlg_info("invalid pkg: Invalid start magic value {}", beginMagic);
		return PackageStatus::invalid;
	}

	// here we know that the package is part of a fragmented pkg
	// find the upper bound, i.e. the place it would have in the sorted fragmented_ vector
	// or otherwise the place it should be inserted to
	FragmentedPackage dummy {};
	dummy.header.seq = seqid;
	auto fpkg = std::upper_bound(fragmented_.begin(), fragmented_.end(), dummy,
		[](const auto& a, const auto& b) { return a.header.seq < b.header.seq; });

	// check if fragmented already contains the given package, otherwise create it
	// try to use an unused pkg buffer instead of allocating a new one
	if(fpkg == fragmented_.end() || fpkg->header.seq != seqid) {
		fpkg = fragmented_.emplace(fpkg);
		fpkg->firstSeen = Clock::now();
		if(!unusedPkgBuffers_.empty()) {
			fpkg->data = std::move(unusedPkgBuffers_.back());
			fpkg->data.clear();
			unusedPkgBuffers_.pop_back();
		}
	}

	// set its header (if it is valid i.e. this package had one)
	if(header) fpkg->header = *header;

	// the required size of fpkg->received
	auto neededSize = fragpart;
	if(endMagic == magic::another)
		++neededSize;

	// resize, store in received
	if(fpkg->received.size() < neededSize)
		fpkg->received.resize(neededSize, false);
	fpkg->received[fragpart] = true;

	// make sure fpkg->data is large enough so that we can copy
	// our data into it
	auto prev = fragpart ? firstDataSize + (fragpart - 1) * fragDataSize : 0u;
	auto bufferBegin = prev;
	auto bufferEnd = static_cast<unsigned int>(dataEnd - dataBegin) + prev;

	if(fpkg->data.size() < bufferEnd)
		fpkg->data.resize(neededSize);

	// TODO: do we have to make sure the fragment was not already received?
	// copy the raw data into the buffer
	std::memcpy(&fpkg->data[bufferBegin], dataBegin, dataEnd - dataBegin);

	// if the fragmented package is complete now, handle it
	if(std::all_of(fpkg->received.begin(), fpkg->received.end(), [](auto b) { return b; })) {
		// try to handle its header
		auto lastAck = remoteSeq_;
		auto processed = processHeader(fpkg->header);
		if(processed != MessageHeaderStatus::valid) {
			dlg_info("invalid pkg: processing frag message header failed: {}", name(processed));
			return PackageStatus::invalid;
		}

		RecvBuf buf {};
		buf.current = fpkg->data.data();
		buf.end = buf.current + fpkg->data.size();
		auto ret = handlePackageData(lastAck, fpkg->header.seq, buf) ?
			PackageStatus::message :
			PackageStatus::invalidMessage;
		unusedPkgBuffers_.push_back(std::move(fpkg->data));
		fragmented_.erase(fpkg);
		return ret;
	}

	return PackageStatus::fragment;
}

bool MessageManager::handlePackageData(uint32_t lastAck, uint32_t seqNumber, RecvBuf buffer)
{
	((void) seqNumber); // unused; may be used for a future message group spec

	dlg_tags("MessageManager", "handlePackageData");

	// there is no sense in processing if there is no message handler
	if(!messageHandler_) {
		dlg_warn("no message handler set");
		return false;
	}

	try {
		// groupSeq will hold the current sequence number the handled
		// messages belong to
		// groupSize holds the left size in the current message group
		// the package data starts with the sequence number of the first message
		// group.
		auto groupSeq = 0u;
		auto groupSize = 0u;
		auto groupFirst = true;

		// we read until the entire (!) package data is processed (without any paddings)
		// or until something goes wrong
		while(buffer.current < buffer.end) {

			// TODO: some group seq number validity check
			// e.g. check the jump is not too high

			// read information forn ext message group
			auto nextSeq = read<uint32_t>(buffer);
			if(!groupFirst && nextSeq == groupSeq) {
				dlg_warn("invalid pkg: two groups with the same seq number {}", groupSeq);
				return false;
			}

			groupFirst = false;
			groupSeq = nextSeq;
			groupSize = read<uint32_t>(buffer);

			// check if given group size would exceed the overall package data size
			if(groupSize == 0 || buffer.current + groupSize > buffer.end) {
				dlg_warn("invalid pkg: group size {} is too large", groupSize);
				return false;
			}

			// if we have already received this message group, simply skip it
			// now the knowledge of the groups size in bytes comes in really handy
			if(lastAck - groupSeq < maxSeqDiff) {
				dlg_debug("skipping already received group {}", groupSeq);
				buffer.current += groupSize;
				continue;
			}

			// handle all messages in the group
			auto groupBuffer = RecvBuf {buffer.current, buffer.current + groupSize};
			while(groupBuffer.current < groupBuffer.end) {
				try {
					auto before = groupBuffer.current;
					auto ret = false;
					ret = messageHandler_(groupSeq, groupBuffer);

					if(!ret) {
						dlg_warn("invalid pkg: handler return false");
						return false;
					}

					// avoid infinite loop, message handler MUST advance buffer
					if(before == groupBuffer.current) {
						dlg_error("invalid messages handler did not advance buffer");
						return false;
					}
				} catch(const RecvBufInvalid& err) {
					dlg_warn("invalid pkg: messageHandler threw: {}", err.what());
					return false;
				}
			}

			// check that the groupBuffer was only advanced in its bounds
			if(groupBuffer.current > groupBuffer.end) {
				dlg_error("message group buffer advanced too far");
				return false;
			}

			// apply the advance of the message group to the data buffer
			buffer.current = groupBuffer.current;

		}
	} catch(const RecvBufOutOfRange& err) {
		// this extra catch is only for our own code above, the
		// message handler has its own try/catch
		// if we land here some assumption about the anatomy of
		// the pacakge was false, i.e. the package is garbage
		dlg_error("invalid pkg size, internal assumptions not met");
		return false;
	}

	// check that buffer was only advanced in bounds
	if(buffer.current > buffer.end) {
		dlg_error("pkg buffer advanced too far");
		return false;
	}

	return true;
}

MessageManager::MessageHandler MessageManager::messageHandler(MessageHandler newHandler)
{
	dlg_debug("setting new message handler");
	std::swap(messageHandler_, newHandler);
	return newHandler;
}

void MessageManager::shrink()
{
	unusedMsgBuffers_ = {};
	unusedPkgBuffers_ = {};
	buffers_.shrink_to_fit();
	packageBuffer_.shrink_to_fit();
	nonCritical_.shrink_to_fit();
	critical_.shrink_to_fit();
	fragmented_.shrink_to_fit();
}

unsigned int MessageManager::discardFragments(Clock::duration age)
{
	auto prev = fragmented_.size();
	auto now = Clock::now();
	std::remove_if(fragmented_.begin(), fragmented_.end(), [&](const auto& pkg) {
		return (now - pkg.firstSeen) >= age;
	});

	auto ret = fragmented_.size() - prev;
	dlg_debug("Discarding {} fragmented packages", ret);
	return ret;
}

} // namespace tkn

