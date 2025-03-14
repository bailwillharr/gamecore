#include <string>
#include <iostream>
#include <span>

#include <asio.hpp>

#include <gamecore/gc_app.h>
#include <gamecore/gc_window.h>
#include <gamecore/gc_vulkan_renderer.h>

// an echo server

static asio::awaitable<void> echo()
{
    constexpr asio::ip::port_type SERVER_PORT = 1234;

    /* shortcut to use error code with co_await */
    static const auto ec_awaitable = asio::as_tuple(asio::use_awaitable);

    /* get io_context for this coroutine (passed to co_spawn) */
    const asio::any_io_executor ctx = co_await asio::this_coro::executor;

    /* Get endpoint for the server (bound to 0.0.0.0 port 1234) */
    const asio::ip::tcp::endpoint server_endpoint(asio::ip::tcp::v4(), SERVER_PORT);

    for (;;) {
        /* acceptor object can asynchronously wait for a client to connect */
        asio::ip::tcp::acceptor acceptor(ctx, server_endpoint);

        GC_INFO("Waiting for connection...");

        /* returns a socket to communicate with a client */
        auto [ec, sock] = co_await acceptor.async_accept(ctx, ec_awaitable);
        if (ec) {
            gc::abortGame("acceptor.async_accept() error: {}", ec.message());
        }

        GC_INFO("Remote connected.");

        for (;;) {
            std::array<char, 512> buf{};
            auto [ec2, sz] = co_await sock.async_read_some(asio::buffer(buf.data(), buf.size()), ec_awaitable);
            if (ec2 == asio::error::eof) {
                GC_INFO("Remote disconnected.");
                break;
            }
            else if (ec2) {
                gc::abortGame("sock.async_read_some() error: {}", ec2.message());
            }

            size_t bytes_remaining = sz;
            while (bytes_remaining > 0) {
                auto [ec3, bytes_written] = co_await sock.async_write_some(asio::buffer(buf.data() + sz - bytes_remaining, bytes_remaining), ec_awaitable);
                if (ec3) {
                    gc::abortGame("sock.async_write_some() error: {}", ec3.message());
                }
                bytes_remaining -= bytes_written;
            }
        }
    }
}

int main()
{
    gc::App::initialise();

    asio::io_context ctx;
    co_spawn(ctx, echo(), asio::detached);

    gc::app().window().setWindowVisibility(true);

    while (!gc::app().window().shouldQuit()) {

        gc::app().vulkanRenderer().waitForPresentFinished();

        gc::app().window().processEvents();

        if (gc::app().window().getKeyPress(SDL_SCANCODE_F11)) {
            gc::app().window().setSize(0, 0, !gc::app().window().getIsFullscreen());
        }
        if (gc::app().window().getKeyPress(SDL_SCANCODE_ESCAPE)) {
            gc::app().window().setQuitFlag();
        }

        ctx.poll();

        gc::app().vulkanRenderer().acquireAndPresent({});
    }

    // ctx.run(); // block until echo() has returned
    gc::App::shutdown();
}