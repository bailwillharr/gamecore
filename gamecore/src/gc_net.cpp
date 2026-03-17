#include "gamecore/gc_net.h"

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <atomic>
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

static asio::awaitable<void> clientSession(asio::ip::tcp::socket socket)
{
    constexpr auto token = asio::as_tuple(asio::use_awaitable);

    asio::error_code ec{};

    auto remote = socket.remote_endpoint(ec);
    if (ec) {
        GC_ERROR("Error getting remote endpoint: {}", ec.message());
        co_return;
    }
    GC_INFO("New session: {}:{}", remote.address().to_string(), remote.port());

    size_t bytes_read = 0;
    std::vector<char> data(1024);

    while (true) {
        asio::streambuf buf;
        std::tie(ec, bytes_read) = co_await asio::async_read_until(socket, buf, '\n', token);
        if (ec) {
            if (ec != asio::error::eof) {
                GC_ERROR("Read error: {}", ec.message());
            }
            break;
        }

        const std::string_view line(static_cast<const char*>(buf.data().data()), buf.size());
        buf.consume(buf.size());

        if (line == "exit\n") {
            break;
        }
        else if (line == "shutdown\n") {
            // TODO: shutdown code here
            break;
        }

        const auto message = std::format("Client sent {} bytes of data!\n", bytes_read);
        size_t bytes_written{};
        std::tie(ec, bytes_written) = co_await asio::async_write(socket, asio::buffer(message), token);
        if (ec) {
            GC_ERROR("Write error: {}", ec.message());
            break;
        }
    }

    GC_INFO("Closing session: {}:{}", remote.address().to_string(), remote.port());
}

static asio::awaitable<void> waitForConnections(const asio::ip::tcp::endpoint& endpoint)
{
    constexpr auto token = asio::as_tuple(asio::use_awaitable);

    asio::error_code ec{};
    auto executor = co_await asio::this_coro::executor;

    asio::ip::tcp::acceptor acceptor(executor, endpoint);

    auto shutdown = std::make_shared<std::atomic_bool>(false);
    while (!shutdown->load(std::memory_order_relaxed)) {
        asio::ip::tcp::socket socket(executor);

        std::tie(ec) = co_await acceptor.async_accept(socket, token);
        if (ec) {
            GC_ERROR("Connection accept error: {}", ec.message());
            continue;
        }

        asio::co_spawn(executor, clientSession(std::move(socket)), asio::detached);
    }
}

Net::Net() : m_context(std::make_shared<asio::io_context>()) {}

void Net::startServer()
{
    const asio::ip::tcp::endpoint endpoint(asio::ip::address(), 6969);

    asio::co_spawn(*m_context, waitForConnections(endpoint), asio::detached);

    m_server_thread = std::jthread([](std::shared_ptr<asio::io_context> context) { context->run(); }, m_context);
}

} // namespace gc
