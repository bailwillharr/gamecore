#include <string>
#include <iostream>
#include <span>

#include <asio.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_vulkan_renderer.h>

// an echo server

constexpr asio::ip::port_type SERVER_PORT = 1234;

static asio::awaitable<void> echo()
{
    /* get io_context for this coroutine (passed to co_spawn) */
    const asio::any_io_executor ctx = co_await asio::this_coro::executor;

    /* Get endpoint for the server (bound to 0.0.0.0 port 1234) */
    const asio::ip::tcp::endpoint server_endpoint(asio::ip::tcp::v4(), SERVER_PORT);

    for (;;) {
        /* acceptor object can asynchronously wait for a client to connect */
        asio::ip::tcp::acceptor acceptor(ctx, server_endpoint);

        std::cout << "Waiting for client to connect...\n";

        /* returns a socket to communicate with a client */
        asio::ip::tcp::socket sock = co_await acceptor.async_accept(ctx, asio::use_awaitable);

        std::cout << "Connected!\n";

        while (sock.is_open()) {
            std::array<char, 512> buf{};
            const size_t sz = co_await sock.async_read_some(asio::buffer(buf.data(), buf.size()), asio::use_awaitable);

            std::cout << "Received " << sz << " bytes of data\n";

            std::cout << "Received string: " << std::string_view(buf.begin(), buf.end()) << "\n";

            size_t bytes_remaining = sz;
            while (bytes_remaining > 0) {
                const size_t bytes_written =
                    co_await sock.async_write_some(asio::buffer(buf.data() + sz - bytes_remaining, bytes_remaining), asio::use_awaitable);
                std::cout << "Wrote " << bytes_written << " bytes\n";
                bytes_remaining -= bytes_written;
            }
        }

        std::cout << "Closed connection\n";
    }
}

int main()
{
    gc::App::initialise();

    asio::io_context ctx;
    co_spawn(ctx, echo(), asio::detached);

    gc::app().window().setWindowVisibility(true);

    while (!gc::app().window().shouldQuit()) {

        gc::app().vulkanRenderer().waitForRenderFinished();

        gc::app().window().processEvents();

        if (gc::app().window().getKeyPress(SDL_SCANCODE_F11)) {
            gc::app().window().setSize(0, 0, !gc::app().window().getIsFullscreen());
        }
        if (gc::app().window().getKeyPress(SDL_SCANCODE_ESCAPE)) {
            gc::app().window().setQuitFlag();
        }

        auto count = ctx.poll();
        if (count > 0) {
            std::cout << "Poll ran " << count << " handlers\n";
        }

        gc::app().vulkanRenderer().acquireAndPresent({});
    }

    // ctx.run(); // block until echo() has returned
    gc::App::shutdown();
}