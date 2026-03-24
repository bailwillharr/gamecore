#include "gamecore/gc_net.h"

#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <thread>

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

#include "gamecore/gc_asio_throw_exception.h"
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

Net::Net() {}

void Net::startServer() { m_server.start(); }

void Net::stopServer() { m_server.stop(); }

bool Net::pollEvents(NetEvent& ev) { return m_event_queue.pop(ev); }

} // namespace gc
