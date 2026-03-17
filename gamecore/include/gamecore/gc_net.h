#pragma once

#include <thread>
#include <memory>

#include <asio/io_context.hpp>

namespace gc {

class Net {
    std::shared_ptr<asio::io_context> m_context{};
    std::jthread m_server_thread;

public:
    Net();

    void startServer();
};

} // namespace gc
