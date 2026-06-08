#include "gamecore/gc_net.h"

#include <asio/error.hpp>
#include <asio/io_context.hpp>

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
#include <variant>

#include "gamecore/gc_asio_throw_exception.h"
#include "gamecore/gc_assert.h"
#include "gamecore/gc_logger.h"
#include "gamecore/gc_net_client.h"

namespace gc {

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

bool Net::startServer(asio::ip::udp::endpoint endpoint)
{
    if (!std::holds_alternative<std::monostate>(m_server_client)) {
        GC_ERROR("Cannot start server if already running as a client or server");
        return false;
    }

    m_server_client.emplace<NetServer>();

    if (!getServer().start(std::move(endpoint))) {
        return false;
    }

    return true;
}

void Net::stopServer()
{
    getServer().stop();
    m_server_client.emplace<std::monostate>();
}

bool Net::connectToServer(const asio::ip::udp::endpoint& endpoint)
{
    if (!std::holds_alternative<std::monostate>(m_server_client)) {
        GC_ERROR("Cannot connect to a server if already running as a client or server");
        return false;
    }

    m_server_client.emplace<NetClient>();

    if (!getClient().connect(endpoint)) {
        return false;
    }

    return true;
}

void Net::disconnectFromServer()
{
    getClient().disconnect();
    m_server_client.emplace<std::monostate>();
}

bool Net::pollEvents(NetEvent& ev)
{
    if (std::holds_alternative<NetServer>(m_server_client)) {
        auto& server = getServer();
        if (server.isRunning()) {
            return server.poll(ev);
        }
        else {
            m_server_client.emplace<std::monostate>();
            return false;
        }
    }
    else if (std::holds_alternative<NetClient>(m_server_client)) {
        auto& client = getClient();
        NetClientConnectionStatus status = client.getConnectionStatus();
        if (status == NetClientConnectionStatus::CONNECTED) {
            return client.poll(ev);
        }
        else if (status == NetClientConnectionStatus::CONNECTING) {
            return false;
        }
        else { // DISCONNECTED
            m_server_client.emplace<std::monostate>();
            return false;
        }
    }
    else {
        return false;
    }
}

void Net::postEvent(NetEvent ev, NetSessionToken session_token)
{
    if (std::holds_alternative<NetServer>(m_server_client)) {
        auto& server = getServer();
        std::vector<uint8_t> buf(4 + ev.data.size());
        NetByteWriter writer(buf);
        writer.writeU32(ev.type.getHash());
        writer.writeBytes(ev.data);
        server.sendMessage(1, std::move(buf), session_token);
    }
    else if (std::holds_alternative<NetClient>(m_server_client)) {
        auto& client = getClient();
        std::vector<uint8_t> buf(4 + ev.data.size());
        NetByteWriter writer(buf);
        writer.writeU32(ev.type.getHash());
        writer.writeBytes(ev.data);
        client.sendMessage(1, std::move(buf));
    }
}

NetMode Net::getMode() const
{
    if (std::holds_alternative<NetServer>(m_server_client)) {
        return NetMode::SERVER;
    }
    else if (std::holds_alternative<NetClient>(m_server_client)) {
        return NetMode::CLIENT;
    }
    else {
        return NetMode::DISCONNECTED;
    }
}

uint32_t Net::getRemoteCount() const
{
    if (std::holds_alternative<NetServer>(m_server_client)) {
        return getServer().getActiveSessionsCount();
    }
    else if (std::holds_alternative<NetClient>(m_server_client)) {
        return 1;
    }
    else {
        return 0;
    }
}

std::vector<NetSessionToken> Net::getRemoteSessions() const
{
    if (std::holds_alternative<NetServer>(m_server_client)) {
        return getServer().getActiveSessionsList();
    }
    else {
        return {};
    }
}

NetClientConnectionStatus Net::getClientConnectionStatus() const { return getClient().getConnectionStatus(); }

asio::ip::udp::endpoint Net::getServerEndpoint() const { return getServer().getLocalEndpoint(); }

std::optional<asio::ip::udp::endpoint> Net::resolve(std::string_view host, std::string_view service)
{
    asio::error_code ec{};
    asio::io_context ctx{};
    asio::ip::udp::resolver resolver(ctx);
    const auto result = resolver.resolve(host, service, ec);
    if (ec) {
        GC_ERROR("resolve error: {}", ec.message());
        return std::nullopt;
    }
    if (result.empty()) {
        return std::nullopt;
    }
    return result.begin()->endpoint();
}

NetServer& Net::getServer()
{
    GC_ASSERT(std::holds_alternative<NetServer>(m_server_client));
    return std::get<NetServer>(m_server_client);
}

const NetServer& Net::getServer() const
{
    GC_ASSERT(std::holds_alternative<NetServer>(m_server_client));
    return std::get<NetServer>(m_server_client);
}

NetClient& Net::getClient()
{
    GC_ASSERT(std::holds_alternative<NetClient>(m_server_client));
    return std::get<NetClient>(m_server_client);
}

const NetClient& Net::getClient() const
{
    GC_ASSERT(std::holds_alternative<NetClient>(m_server_client));
    return std::get<NetClient>(m_server_client);
}

} // namespace gc
