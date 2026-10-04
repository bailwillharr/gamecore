#include "gamecore/gc_net.h"

#include <variant>

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include <gclog/gclog.h>

#include "gamecore/gc_assert.h"

namespace gc {

bool Net::startServer(asio::ip::udp::endpoint endpoint)
{
    if (!std::holds_alternative<std::monostate>(m_server_client)) {
        GC_ERROR("Cannot start server if already running as a client or server");
        return false;
    }

    auto& server = m_server_client.emplace<NetServer>();
    server.setSimConfig(m_sim_config);
    if (!server.start(std::move(endpoint))) {
        reset();
        return false;
    }

    return true;
}

void Net::stopServer()
{
    if (getServer()) {
        reset();
    }
}

void Net::disconnectPeer(NetPeerId peer)
{
    if (NetServer* server = getServer()) {
        server->disconnectPeer(peer);
    }
}

bool Net::connectToServer(const asio::ip::udp::endpoint& endpoint)
{
    if (!std::holds_alternative<std::monostate>(m_server_client)) {
        GC_ERROR("Cannot connect to a server if already running as a client or server");
        return false;
    }

    auto& client = m_server_client.emplace<NetClient>();
    client.setSimConfig(m_sim_config);
    if (!client.connect(endpoint)) {
        reset();
        return false;
    }

    return true;
}

void Net::disconnectFromServer()
{
    if (getClient()) {
        reset();
    }
}

NetClientConnectionStatus Net::getClientConnectionStatus() const
{
    if (const NetClient* client = getClient()) {
        return client->getConnectionStatus();
    }
    else {
        return NetClientConnectionStatus::DISCONNECTED;
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

asio::ip::udp::endpoint Net::getServerEndpoint() const
{
    if (const NetServer* server = getServer()) {
        return server->getLocalEndpoint();
    }
    else if (const NetClient* client = getClient()) {
        return client->getServerEndpoint();
    }
    else {
        return {};
    }
}

NetPeerId Net::getLocalPeerId() const
{
    if (getServer()) {
        return NET_PEER_SERVER;
    }
    else if (const NetClient* client = getClient()) {
        return client->getLocalPeerId();
    }
    else {
        return NET_PEER_NONE;
    }
}

uint32_t Net::getRemoteCount() const
{
    if (const NetServer* server = getServer()) {
        return server->getPeerCount();
    }
    else if (const NetClient* client = getClient()) {
        return (client->getConnectionStatus() == NetClientConnectionStatus::CONNECTED) ? 1 : 0;
    }
    else {
        return 0;
    }
}

std::vector<NetPeerInfo> Net::getPeers() const
{
    if (const NetServer* server = getServer()) {
        return server->getPeers();
    }
    else if (const NetClient* client = getClient(); client && client->getConnectionStatus() == NetClientConnectionStatus::CONNECTED) {
        return {NetPeerInfo{.id = NET_PEER_SERVER, .endpoint = client->getServerEndpoint(), .stats = client->getStats()}};
    }
    else {
        return {};
    }
}

NetDisconnectReason Net::getLastDisconnectReason() const { return m_last_disconnect_reason; }

bool Net::pollEvents(NetEvent& ev)
{
    bool has_event = false;
    if (!m_pending_events.empty()) {
        ev = std::move(m_pending_events.front());
        m_pending_events.pop_front();
        has_event = true;
    }
    else if (NetServer* server = getServer()) {
        has_event = server->poll(ev);
        if (!has_event && !server->isRunning()) {
            reset(); // the server stopped itself
        }
    }
    else if (NetClient* client = getClient()) {
        has_event = client->poll(ev);
        if (!has_event && client->getConnectionStatus() == NetClientConnectionStatus::DISCONNECTED) {
            reset(); // failed to connect, or the connection was lost
        }
    }

    if (has_event && ev.kind == NetEventKind::DISCONNECTED && ev.peer == NET_PEER_SERVER) {
        m_last_disconnect_reason = ev.reason;
    }
    return has_event;
}

void Net::postEvent(const NetEvent& ev, NetDelivery delivery, NetPeerId peer)
{
    if (NetServer* server = getServer()) {
        server->sendMessage(encodeNetEvent(ev), delivery, peer);
    }
    else if (NetClient* client = getClient()) {
        client->sendMessage(encodeNetEvent(ev), delivery);
    }
}

void Net::setSimConfig(const NetSimConfig& config)
{
    m_sim_config = config;
    if (NetServer* server = getServer()) {
        server->setSimConfig(config);
    }
    else if (NetClient* client = getClient()) {
        client->setSimConfig(config);
    }
}

NetSimConfig Net::getSimConfig() const { return m_sim_config; }

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

NetServer* Net::getServer() { return std::get_if<NetServer>(&m_server_client); }

const NetServer* Net::getServer() const { return std::get_if<NetServer>(&m_server_client); }

NetClient* Net::getClient() { return std::get_if<NetClient>(&m_server_client); }

const NetClient* Net::getClient() const { return std::get_if<NetClient>(&m_server_client); }

void Net::reset()
{
    NetEvent ev{};
    if (NetServer* server = getServer()) {
        server->stop();
        while (server->poll(ev)) {
            m_pending_events.push_back(std::move(ev));
        }
    }
    else if (NetClient* client = getClient()) {
        client->disconnect();
        while (client->poll(ev)) {
            m_pending_events.push_back(std::move(ev));
        }
    }
    m_server_client.emplace<std::monostate>();
}

} // namespace gc
