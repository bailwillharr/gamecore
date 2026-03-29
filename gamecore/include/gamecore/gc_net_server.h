#pragma once

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>
#include <asio/awaitable.hpp>

#include <mutex>
#include <unordered_map>
#include <thread>

namespace gc {

class NetServer {
    asio::io_context m_context;
    std::mutex m_sessions_mutex{};
    std::jthread m_server_thread{};

public:
    ~NetServer();

    bool start(asio::ip::udp::endpoint endpoint);
    void stop();

    bool isRunning() const;

private:
    asio::awaitable<void> serverLoop(asio::ip::udp::endpoint endpoint);
};

} // namespace gc
