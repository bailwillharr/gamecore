#include "gamecore/gc_net.h"

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <thread>

#include <asio/awaitable.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/udp.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/detached.hpp>
#include <asio/co_spawn.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/write.hpp>
#include <asio/as_tuple.hpp>
#include <asio/streambuf.hpp>

#include "gamecore/gc_asio_throw_exception.h"
#include "gamecore/gc_logger.h"

namespace gc {

static asio::awaitable<void> serverLoop(asio::ip::udp::endpoint endpoint)
{
    constexpr auto token = asio::as_tuple(asio::use_awaitable);

    asio::error_code ec{};
    auto executor = co_await asio::this_coro::executor;

    asio::ip::udp::socket socket(executor);

    socket.open(endpoint.protocol(), ec);
    if (ec) {
        GC_ERROR("Failed to open socket: {}", ec.message());
        co_return;
    }

    socket.bind(endpoint, ec);
    if (ec) {
        GC_ERROR("Failed to bind socket: {}", ec.message());
        co_return;
    }

    GC_INFO("Starting server on {}:{}", endpoint.address().to_string(), endpoint.port());

    for (;;) {
        size_t bytes_read{};
        std::array<char, 8> buf{};
        asio::ip::udp::endpoint client_endpoint{};
        std::tie(ec, bytes_read) = co_await socket.async_receive_from(asio::buffer(buf), client_endpoint, token);
        if (ec) {
            GC_ERROR("Failed to receive on socket: {}", ec.message());
            continue;
        }

        size_t bytes_written{};
        auto send_buf = std::format("You sent {} bytes", bytes_read);
        std::tie(ec, bytes_written) = co_await socket.async_send_to(asio::buffer(send_buf), client_endpoint, token);
        if (ec) {
            GC_ERROR("Failed to send on socket: {}", ec.message());
            continue;
        }
        if (bytes_written < send_buf.size()) {
            GC_ERROR("Didn't send all data. Sent {}/{}", bytes_written, send_buf.size());
        }
    }
}

void NetEventQueue::push(NetEvent event)
{
    std::scoped_lock lock(m_mutex);
    m_queue.push(std::move(event));
}

bool NetEventQueue::pop(NetEvent& ev)
{
    std::scoped_lock lock(m_mutex);
    if (m_queue.empty()) {
        return false;
    }
    else {
        ev = m_queue.front();
        m_queue.pop();
        return true;
    }
}

Net::Net() {}

void Net::startServer()
{
    m_server.server_thread = std::jthread(
        [](asio::io_context* context) {
            asio::ip::udp::endpoint endpoint(asio::ip::address(), 6969);
            asio::co_spawn(*context, serverLoop(endpoint), asio::detached);
            context->run();
        },
        &m_context);
}

void Net::stop() { m_context.stop(); }

bool Net::pollEvents(NetEvent& ev) { return m_event_queue.pop(ev); }

} // namespace gc
