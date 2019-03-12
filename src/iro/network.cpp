#include "network.hpp"
#include <dlg/dlg.hpp>
#include <stage/bits.hpp>

// TODO:
// - invalid packages (currently using doi::read, only asserts size)
//   probably best solved by exceptions in read function, caught here
// - acknowledge and stuff

Socket::Socket() {
	auto ep = udp::endpoint(asio::ip::address_v4({127, 0, 0, 1}), port);
	try {
		socket_ = {ioService_, ep};
		ep = udp::endpoint(asio::ip::address_v4({127, 0, 0, 1}), port + 1);
		socket().connect(ep);

		// wait for other side
		socket().wait(asio::ip::udp::socket::wait_read);
		std::vector<std::byte> buf(socket().available());
		socket().receive(asio::buffer(buf.data(), buf.size()));

		auto span = nytl::Span<const std::byte>(buf);
		dlg_assert(buf.size() == 4);
		dlg_assert(doi::read<std::uint32_t>(span) == 42);
		dlg_info("connection established as 0");
	} catch(std::system_error& exception) {
		auto nep = udp::endpoint(asio::ip::address_v4({127, 0, 0, 1}), port + 1);
		socket_ = {ioService_, nep};
		socket().connect(ep);

		// send to other side
		auto i = std::uint32_t(42);
		socket().send(asio::buffer(&i, 4));
		dlg_info("connection established as 1");
	}

	// Not sure really why this is needed though to make socket().available()
	// work...
	socket().non_blocking(true);
}

void Socket::update(MsgHandler handler) {
	std::error_code ec {};
	auto a = socket().available(ec);
	if(a == 0) {
		if(ec) {
			dlg_warn("{}", ec.message());
		}
		return;
	}

	std::vector<std::byte> buf(a);
	socket().receive(asio::buffer(buf.data(), buf.size()));
	auto recv = RecvBuf(buf);
	while(!recv.empty()) {
		handler(recv);
	}
}

void Socket::nextStep() {
	auto& d = pending_.data;
	if(d.empty()) {
		return;
	}

	socket().send(asio::buffer(d.data(), d.size()));
	pending_.data.clear();
}
