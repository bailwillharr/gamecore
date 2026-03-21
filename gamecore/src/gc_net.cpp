#include "gamecore/gc_net.h"

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <string_view>
#include <memory>
#include <thread>

#include <asio/awaitable.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
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

static asio::awaitable<void> clientSession(asio::ip::tcp::socket socket, std::shared_ptr<NetEventQueue> event_queue)
{
    using namespace ::gc::literals;

    constexpr auto token = asio::as_tuple(asio::use_awaitable);

    asio::error_code ec{};

    auto remote = socket.remote_endpoint(ec);
    if (ec) {
        GC_ERROR("Error getting remote endpoint: {}", ec.message());
        co_return;
    }
    GC_INFO("New session: {}:{}", remote.address().to_string(), remote.port());
    const auto hello_message = std::format("Connected to server. Your IP: {}:{}\n", remote.address().to_string(), remote.port());
    std::tie(ec, std::ignore) = co_await asio::async_write(socket, asio::buffer(hello_message), token);
    if (ec) {
        GC_ERROR("Write error: {}", ec.message());
        co_return;
    }

    size_t bytes_read = 0;
    std::vector<char> data(1024);

    while (true) {
        asio::streambuf buf;
        std::tie(ec, bytes_read) = co_await asio::async_read_until(socket, buf, '\n', token);
        if (ec) {
            if (ec != asio::error::eof) {
                GC_ERROR("Read error: {}", ec.message());
            }
            else {
                GC_INFO("Client sent EOF");
            }
            break;
        }

        const std::string_view line(static_cast<const char*>(buf.data().data()), buf.size());
        buf.consume(buf.size());

        if (line == "exit\n") {
            const std::string exit_message = "Goodbye\n";
            std::tie(ec, std::ignore) = co_await asio::async_write(socket, asio::buffer(exit_message), token);
            if (ec) {
                GC_ERROR("Write error: {}", ec.message());
                break;
            }

            socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
            if (ec) {
                GC_ERROR("Failed to call socket.shutdown(): {}", ec.message());
            }

            break;
        }
        else if (line == "shutdown\n") {
            event_queue->push(NetEvent{"shutdown"_name});
        }
    }

    GC_INFO("Closing session: {}:{}", remote.address().to_string(), remote.port());

    // socket will go out of scope and close here
}

static asio::awaitable<void> waitForConnections(asio::ip::tcp::endpoint endpoint, std::shared_ptr<NetEventQueue> event_queue)
{
    constexpr auto token = asio::as_tuple(asio::use_awaitable);

    asio::error_code ec{};
    auto executor = co_await asio::this_coro::executor;

    asio::ip::tcp::acceptor acceptor(executor);

    acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        GC_ERROR("Failed to open acceptor: {}", ec.message());
        co_return;
    }

    constexpr int MAX_BIND_ATTEMPTS = 3;
    for (int i = 0; i < MAX_BIND_ATTEMPTS; ++i) {
        acceptor.bind(endpoint, ec);
        if (ec) {
            if (ec == asio::error::address_in_use) {
                GC_WARN("Address in use: {}:{}", endpoint.address().to_string(), endpoint.port());
                endpoint = asio::ip::tcp::endpoint(endpoint.address(), endpoint.port() + 1);
                continue;
            }
            else {
                GC_ERROR("Failed to bind acceptor: {}", ec.message());
                co_return;
            }
        }
        else {
            break;
        }
    }

    GC_INFO("Starting server on {}:{}", endpoint.address().to_string(), endpoint.port());

    acceptor.listen(acceptor.max_listen_connections, ec);
    if (ec) {
        GC_ERROR("Failed to listen on acceptor: {}", ec.message());
        co_return;
    }

    for (;;) {
        asio::ip::tcp::socket socket(executor);

        std::tie(ec) = co_await acceptor.async_accept(socket, token);
        if (ec) {
            GC_ERROR("Acceptor failed to accept: {}", ec.message());
            continue;
        }

        asio::co_spawn(executor, clientSession(std::move(socket), event_queue), asio::detached);
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

Net::Net() : m_context(std::make_shared<asio::io_context>()), m_event_queue(std::make_shared<NetEventQueue>()) {}

void Net::startServer()
{
    m_server_thread = std::jthread(
        [](std::shared_ptr<asio::io_context> context, std::shared_ptr<NetEventQueue> event_queue) {
            asio::ip::tcp::endpoint endpoint(asio::ip::address(), 6969);
            asio::co_spawn(*context, waitForConnections(endpoint, event_queue), asio::detached);
            context->run();
        },
        m_context, m_event_queue);
}

void Net::stopServer() { m_context->stop(); }

bool Net::pollEvents(NetEvent& ev) { return m_event_queue->pop(ev); }

bool Net::isServerRunning() const { return !m_context->stopped(); }

} // namespace gc
