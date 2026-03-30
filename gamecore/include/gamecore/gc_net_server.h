#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>

#include <mutex>
#include <unordered_map>
#include <thread>

namespace gc {

struct NetServerStatus {
    mutable std::mutex mutex{};
    asio::ip::udp::endpoint local_endpoint{};
};

class NetServer {
    asio::io_context m_context;
    std::jthread m_server_thread{};
    NetServerStatus m_server_status{};

public:
    ~NetServer();

    bool start(asio::ip::udp::endpoint endpoint);
    void stop();

    bool isRunning() const;
    asio::ip::udp::endpoint getLocalEndpoint() const;

private:
    asio::awaitable<void> serverLoop(asio::ip::udp::endpoint endpoint);
};

} // namespace gc
