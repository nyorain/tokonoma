#include "network.hpp"
#include <dlg/dlg.hpp>
#include <stage/bits.hpp>
#include <mutex>
#include <condition_variable>

// TODO:
// - invalid packages (currently using doi::read, only asserts size)
//   probably best solved by exceptions in read function, caught here
// - acknowledge and stuff

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
	ioService_.run();

	player_ = socket().local_endpoint() < socket().remote_endpoint();

	// Not sure really why this is needed though to make socket().available()
	// work...
	socket().non_blocking(true);

	recvd_.resize(2 * delay);
	ownPending_.resize(2 * delay);
	write(sending_, step_); // add dummy for next step
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
	std::error_code ec {};
	auto a = socket().available(ec);
	if(ec) {
		dlg_warn("{}", ec.message());
	}

	while(a > 0) {
		std::vector<std::byte> buf(a);
		socket().receive(asio::buffer(buf.data(), buf.size()));
		++recv_;

		auto r = RecvBuf(buf);
		dlg_assert(doi::read<std::uint64_t>(r) == recv_);

		recvd_[(recv_ + delay) % (2 * delay)] = {std::move(buf)};

		// otherwise the other side has made a step they didn't
		// even have our input for?! critical protocol error
		dlg_assert(recv_ < step_ + delay);

		a = socket().available(ec);
		if(ec) {
			dlg_warn("{}", ec.message());
		}
	}

	// next step?
	if(step_ >= recv_ + delay) {
		dlg_info("no update: {} vs {}", step_, recv_);
		return false;
	}

	// send pending
	auto& d = sending_.data;
	// dlg_trace("sending {} bytes", d.size());
	socket().send(asio::buffer(d.data(), d.size()));
	ownPending_[(step_ + delay) % (2 * delay)] = std::move(sending_);
	++step_;
	write(sending_, step_);

	// process
	auto recv = RecvBuf(recvd_[step_ % (2 * delay)].data);
	if(!recv.empty()) { // only at the beginning. assert that?
		auto i = doi::read<std::uint64_t>(recv);
		dlg_assertm(i + delay == step_, "{} {}", i, step_);
		while(!recv.empty()) {
			handler(1 - player_, recv);
		}
	}

	auto own = RecvBuf(ownPending_[step_ % (2 * delay)].data);
	if(!own.empty()) { // only at the beginning. assert that?
		auto i = doi::read<std::uint64_t>(own);
		dlg_assertm(i + delay == step_, "{} {}", i, step_);
		while(!own.empty()) {
			handler(player_, own);
		}
	}

	return true;
}
