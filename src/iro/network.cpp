#include "network.hpp"
#include <dlg/dlg.hpp>
#include <tkn/bits.hpp>
#include <mutex>
#include <condition_variable>
#include <random>

// TODO:
// - invalid packet (currently using tkn::read, only asserts size)
//   probably best solved by exceptions in read function, caught here
// - make packet receiving (header processing, check whether re-send
//   is needed and re-send, if so) async. That can happen in separate
//   thread. Just sync via mutex.
// - when a player knows it's behind (i.e. step < recv) it could execute
//   multiple steps at once, trying to catch up.
//   I guess update() could return a enum status:
//   - no update
//   - computer once
//   - compute and call again
//   But when we do it like this we should probably fix a time/step
//   rate? But that's also bad because then we make assumptions
//   about what clients can deliver. We could fix it dynamically?
//   But the best way would for the faster client to slow down
//   when he detects that he is too fast.
// - fix all comparisons for step logic. Maybe test it by just using
//   an u8 for step_ and recv_ (should be enough anyways, makes
//   packets smaller!)
// - timeouts
// - ping times

struct Header {
	u32 magic; // to minimize chance we handle misguided packets
	u32 step; // the step this message belons to (for sender)
	u32 ack;
};

bool isAckSet(u32 bitset, int offset) {
	dlg_assert(offset >= -int(delay) && offset <= int(delay));
	auto bit = delay + offset;
	return bitset & (u32(1) << bit);
}

void setAckRef(u32& bitset, int offset, bool set = true) {
	dlg_assert(offset >= -int(delay) && offset <= int(delay));
	auto bit = delay + offset;
	auto mask = (u32(1) << bit);
	if(set) {
		bitset |= mask;
	} else {
		bitset &= ~mask;
	}
}

int stepOffset(i64 base, i64 other) {
	i64 diff = other - base;
	if(std::abs(diff) > delay) {
		return base - other;
	} else {
		return diff;
	}
}

std::string printAckBits(u32 bitset) {
	std::string ret;
	for(auto i = 0u; i < 2 * delay; ++i) {
		ret += std::to_string((bitset & (u32(1) << i)) != 0);
	}
	return ret;
}

Socket::Socket() {
	auto bep = udp::endpoint(udp::v4(), broadcastPort);
	broadcast_ = udp::socket(ioService_);
	broadcast_->open(bep.protocol());
	broadcast_->set_option(asio::socket_base::reuse_address(true));
	broadcast_->set_option(asio::socket_base::broadcast(true));
	broadcast_->bind(bep);

	// TODO: bad solution: may not work in all cases (even only local)
	// we could instead somehow get our own ip (to filter out)
	// from the broadcast we send
	udp::resolver resolver(ioService_);
	udp::resolver::query query(udp::v4(), asio::ip::host_name(), "");
	udp::resolver::iterator iter = resolver.resolve(query);
	udp::resolver::iterator end; // End marker.
	dlg_assert(iter != end);
	udp::endpoint sep = *iter;

	// port testing
	for(auto iport = 21345; iport < 65000; ++iport) {
		try {
			auto ep = udp::endpoint(sep.address(), iport);
			socket_ = asio::ip::udp::socket(ioService_, ep);
			break;
		} catch(const std::system_error& exception) {
			continue;
		}
	}

	if(!socket_) {
		throw std::runtime_error("Couldn't open socket");
	}

	dlg_info("socket on {}", socket().local_endpoint());
	socket().set_option(asio::socket_base::broadcast(true));

	// send broadcast
	auto ep = udp::endpoint(asio::ip::address_v4::broadcast(), broadcastPort);
	auto i = std::uint32_t(42);
	socket().send_to(asio::buffer(&i, 4), ep);
	dlg_info("sent broadcast");

	// wait
	// TODO: when our broadcast packet is lost, we will get stuck waiting.
	// send out new broadcasts occasionally (like every 5 seconds)
	// probably possible with an asio timer
	std::uint32_t state = 0;

	udp::endpoint recvep1;
	std::uint32_t recvbuf1;
	auto socketHandler = [&](auto& ec, auto count) {
		if(ec || count < 4) {
			return; // XXX: restart recv?
		}
		this->recvSocket(recvep1, recvbuf1, state);
	};
	socket().async_receive_from(asio::buffer(&recvbuf1, 4), recvep1, socketHandler);

	// wait for another process's broadcast
	udp::endpoint recvep2;
	std::uint32_t recvbuf2;
	auto brdHandler = [&](auto& ec, auto count) {
		if(ec || count < 4) {
			return; // XXX: restart recv?
		}
		this->recvBroadcast(recvep2, recvbuf2, state);
	};
	broadcast_->async_receive_from(asio::buffer(&recvbuf2, 4), recvep2, brdHandler);

	// will run until connected
	// TODO: make this async
	ioService_.run();

	player_ = socket().local_endpoint() < socket().remote_endpoint();

	// Not sure really why this is needed though to make socket().available()
	// work...
	socket().non_blocking(true);

	// The buffers have to be of size 2 * delay, they are basically
	// ring buffers.
	// The worst case (in terms of used buffer slots) is this:
	// the other side can at max be delay steps ahead of this sides'
	// step_. But it may be possible that our packets for step_ already
	// reached the other side (therefore the other side being at step_ + delay),
	// but we still did not get the packets for (step_ - delay + 1), i.e.
	// maxReceieved = step - delay. In that case we might get (id = step_ + delay)
	// before the (2 * delay - 1) nuber of packets before that.
	recvd_.resize(2 * delay);

	// sent_ on the other hand could just be delay in size because
	// we always process our own packets after exactly delay steps.
	// But since we also use ownPending to resend packets, we need to
	// additionally track the packets from furhter before, i.e.
	// back to step_ - 2 * delay. We only know that the other side
	// did get recv_ - delay (since otherwise it could not be at recv_)
	// and we know that recv_ cannot be more than delay steps behind
	// step_.
	sent_.resize(2 * delay);

	Header hdr {packetMagic, step_, recv_};
	write(sending_, hdr); // add dummy for next (first) step
}

void Socket::recvSocket(udp::endpoint& ep, std::uint32_t& num, unsigned& state) {
	if(state == 0) {
		// someone received our broadcast and answers now
		if(num == 43) {
			// answer confirmation
			socket().connect(ep);
			num = 44;
			socket().send(asio::buffer(&num, 4));
			state = 2;
			ioService_.stop();
			dlg_info("received broadcast answer from {}", ep);
			return;
		}
	} else if(state == 1) {
		// we received final confirmation
		if(num == 44) {
			socket().connect(ep);
			state = 2;
			ioService_.stop();
			dlg_info("received confirmation from {}", ep);
			return;
		}
	}

	dlg_warn("invalid message: {}, {}", state, num);

	// continue
	auto socketHandler = [&](auto& ec, auto count) {
		if(ec || count < 4) {
			return; // XXX: restart recv?
		}
		this->recvSocket(ep, num, state);
	};
	socket().async_receive_from(asio::buffer(&num, 4), ep, socketHandler);
}

void Socket::recvBroadcast(udp::endpoint& ep, std::uint32_t& num, unsigned& state) {
	auto brdHandler = [&](auto& ec, auto count) {
		if(ec || count < 4) {
			return; // XXX: restart recv?
		}
		this->recvBroadcast(ep, num, state);
	};

	if(ep == socket().local_endpoint()) {
		dlg_debug("filtering out own broadcast");
		broadcast_->async_receive_from(asio::buffer(&num, 4), ep, brdHandler);
		return;
	}

	if(num == 42) {
		dlg_info("received broadcast from {}", ep);
		// answer confirmation 1
		num = 43;
		socket().send_to(asio::buffer(&num, 4), ep);
		state = 1;
	}
}

bool Socket::update(MsgHandler handler) {
	while(true) {
		std::error_code ec {};
		auto a = socket().available(ec);
		if(ec) {
			dlg_warn("socket.available: {}", ec.message());
			break;
		}

		if(!a) {
			break;
		}

		std::vector<std::byte> buf(a);
		auto size = socket().receive(asio::buffer(buf.data(), buf.size()));
		// dlg_trace("received {} bytes", size);
		buf.resize(size);

		if(size < sizeof(Header)) {
			dlg_info("Received packet is too small: {}", size);
			continue;
		}

		auto r = RecvBuf(buf);
		auto h = tkn::read<Header>(r);

		// check magic
		if(h.magic != packetMagic) {
			// just discard the packet
			dlg_info("Invalid packet magic number: {}", h.magic);
			continue;
		}

		// check step number
		// This can happen when we received old packets
		auto off = stepOffset(step_, h.step);
		// dlg_trace("  step: {} (off {})", h.step, off);
		if(std::abs(off) > int(delay)) {
			// just discard
			dlg_info("Invalid step in packet: {} (step_ = {})",
				h.step, step_);
			continue;
		}

		// check if already received
		if(isAckSet(recv_, off)) {
			dlg_info("Received redundant packet {}", h.step);
			// TODO: use potantially new information in h.ack?
			continue;
		}

		// TODO: We randomly drop valid packets for testing atm.
		static std::mt19937 rgen;
		// rgen.seed(std::time(nullptr));
		std::uniform_real_distribution<float> distr(0.f, 1.f);
		auto lossChance = 0.0;
		if(distr(rgen) < lossChance) {
			dlg_info("dropping step {}", h.step);
			continue;
		}

		// update recv_ bitset, setting the bit for the packet we
		// just received
		setAckRef(recv_, off);

		// update the ack_ bitset
		// We have to bring the h.ack bitset to our step_ base using off
		if(off > 0) {
			h.ack = h.ack << off;
		} else if(off < 0) {
			h.ack = h.ack >> -off;
		}

		// check that ack_ does not contain acks for packets we didn't send.
		dlg_assert((~((1u << delay) - 1) & h.ack) == 0u);

		ack_ |= h.ack;
		recvd_[h.step % (2 * delay)] = {std::move(buf)};
	}

	// TODO: encode potential new ack information in the message headers
	// when we have to re-send.
	// Just modify them inplace, always additive

	// check whether we have to re-send a packet
	// we resend packets in the following situations:
	if(ack_ == 0 && initCount_ >= delay) {
		// ack_ is just empty. In that case the other side is as far
		// behind as possible.
		using namespace std::chrono;
		if(waiting_ && duration_cast<milliseconds>(Clock::now() - *waiting_).count() > 5) { // TODO!
			auto d = sent_[(step_ - delay) % (2 * delay)].data;
			dlg_trace("resending packet (empty) {}", step_ - delay);
			socket().send(asio::buffer(d.data(), d.size()));
		}
	} else {
		// there is a hole in the ack bits we got from the other side.
		// That packet might just be late, but the probability is
		// high is just got lost.
		// We only re-send it for the first hole.
		auto start = std::max(i32(delay) - i32(initCount_), 0);
		auto count = initCount_;
		for(auto i = start; i < i32(start + count); ++i) {
			auto set = ack_ & (1u << i);
			if(!set) {
				dlg_trace("resending packet (hole) {}", step_ - delay + i);
				auto d = sent_[(step_ - delay + i) % (2 * delay)].data;
				socket().send(asio::buffer(d.data(), d.size()));
				break;
			}
		}
	}
	// In any case, if the other side is missing the packet it needs
	// to proceed (offset: -delay) we always re-send that.


	// check whether we can do the next step
	if(initCount_ >= delay && !isAckSet(recv_, -i32(delay))) {
		if(!waiting_) {
			waiting_ = Clock::now();
		}

		dlg_info("no update: {} vs {}", step_, printAckBits(recv_));
		return false;
	}

	waiting_ = {};

	// we can do the next step, yeay!
	// send the accumulated messages
	auto& d = sending_.data;
	// dlg_trace("sending {} bytes", d.size());
	socket().send(asio::buffer(d.data(), d.size()));
	sent_[step_ % (2 * delay)] = std::move(sending_);

	// process
	if(initCount_ >= i32(delay)) {
		auto recv = RecvBuf(recvd_[u32(step_ - delay) % (2 * delay)].data);
		dlg_assert(!recv.empty());

		auto hdr = tkn::read<Header>(recv);
		dlg_assertm(hdr.step + delay == step_, "{} {}", hdr.step, step_);
		while(!recv.empty()) {
			handler(1 - player_, recv);
		}
	}

	if(initCount_ >= i32(delay)) {
		auto own = RecvBuf(sent_[u32(step_ - delay) % (2 * delay)].data);
		dlg_assert(!own.empty());
		auto hdr = tkn::read<Header>(own);
		dlg_assertm(hdr.step + delay == step_, "{} {}", hdr.step, step_);
		while(!own.empty()) {
			handler(player_, own);
		}
	}

	++step_;
	if(initCount_ < 2 * delay) ++initCount_;

	recv_ = recv_ >> 1u;
	ack_ = ack_ >> 1u;

	Header hdr {packetMagic, step_, recv_};
	write(sending_, hdr);

	return true;
}
