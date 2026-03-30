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
    if (m_local_mode != NetMode::DISCONNECTED) {
        GC_ERROR("Cannot start server if already running as a client or server");
        return false;
    }

    m_server_client.emplace<NetServer>();

    if (!getServer().start(std::move(endpoint))) {
        return false;
    }

    m_local_mode = NetMode::SERVER;
    return true;
}

void Net::stopServer()
{
    getServer().stop();
    if (m_local_mode == NetMode::SERVER) {
        m_local_mode = NetMode::DISCONNECTED;
    }
}

bool Net::connectToServer(const asio::ip::udp::endpoint& endpoint)
{
    if (m_local_mode != NetMode::DISCONNECTED) {
        GC_ERROR("Cannot connect to a server if already running as a client or server");
        return false;
    }

    m_server_client.emplace<NetClient>();

    if (!getClient().connect(endpoint)) {
        return false;
    }

    m_local_mode = NetMode::CLIENT;
    return true;
}

void Net::disconnectFromServer()
{
    if (m_local_mode == NetMode::CLIENT) {
        m_local_mode = NetMode::DISCONNECTED;
    }

    getClient().disconnect();
}

bool Net::pollEvents(NetEvent& ev)
{
    switch (m_local_mode) {
    case NetMode::CLIENT:
        if (getClient().getState() == NetClientState::DISCONNECTED) {
            m_local_mode = NetMode::DISCONNECTED;
        }
        return getClient().poll(ev);
    case NetMode::SERVER:
        if (getServer().isRunning() == false) {
            m_local_mode = NetMode::DISCONNECTED;
        }
        return false;
    default:
        return false;
    }
}

NetMode Net::getMode() const { return m_local_mode; }

NetClientState Net::getClientState() const { return getClient().getState(); }

asio::ip::udp::endpoint Net::getServerEndpoint() const { return getServer().getLocalEndpoint(); }

bool Net::isServerRunning() const { return getServer().isRunning(); }

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
