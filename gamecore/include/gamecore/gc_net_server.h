#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>

#include <mutex>
#include <unordered_map>
#include <thread>

namespace gc {

enum class NetServerClientSessionStatus {

};

struct NetServerClientSession {};

class NetServer {
    asio::io_context m_context;
    std::mutex m_sessions_mutex{};
    std::unordered_map<asio::ip::udp::endpoint, NetServerClientSession> m_sessions{};
    std::jthread m_server_thread{};

public:
    void start();
    void stop();

private:
    asio::awaitable<void> serverLoop(asio::ip::udp::endpoint endpoint);
};

} // namespace gc
