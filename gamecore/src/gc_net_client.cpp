#include "gamecore/gc_net_client.h"

#include <random>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/as_tuple.hpp>
#include <asio/steady_timer.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <chrono>

#include "gamecore/gc_net_common.h"

namespace gc {

[[maybe_unused]] static uint32_t generateClientNonce()
{
    std::mt19937 rand32(std::random_device{}());
    return rand32();
}

NetClient::~NetClient() { disconnect(); }

bool NetClient::connect(const asio::ip::udp::endpoint& endpoint)
{
    disconnect(); // just in case
    m_context.restart();
    m_state.store(NetClientConnectionStatus::CONNECTING);
    m_client_thread = std::jthread(
        [](NetClient& self, asio::ip::udp::endpoint endpoint) {
            //asio::co_spawn(self.m_context, self.clientLoop(endpoint), asio::detached);
            (void)endpoint;
            self.m_context.run();
            self.m_state.store(NetClientConnectionStatus::DISCONNECTED);
        },
        std::ref(*this), endpoint);
    return true;
}

void NetClient::disconnect()
{
    m_context.stop();
    m_client_thread = {};
    m_state.store(NetClientConnectionStatus::DISCONNECTED);
}

bool NetClient::poll(NetEvent& ev) { return m_event_queue.pop(ev); }

NetClientConnectionStatus NetClient::getConnectionStatus() const
{
    return m_state.load(); // not sure if relaxed can be used here since this flag can cause the client to be destroyed
}

} // namespace gc
