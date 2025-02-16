#include <asio.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_abort.h>

static void on_connect(const asio::error_code& ec)
{
	GC_INFO("Async connected! ec: {}", ec.message());
}

static void network_test()
{
	asio::error_code ec;
	asio::io_context context;
	asio::ip::tcp::socket socket(context);
	const asio::ip::address address = asio::ip::make_address("10.0.0.101", ec);
	if (ec) {
		gc::abortGame("asio::ip::make_address() error: {}", ec.message());
	}
	const asio::ip::port_type port = 80;
	const asio::ip::tcp::endpoint endpoint(address, port);	
	socket.async_connect(endpoint, on_connect);
	context.run();
}

int main()
{
    gc::App::initialise();

	network_test();

    gc::App::shutdown();
}
