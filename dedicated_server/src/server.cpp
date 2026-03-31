#include "server.h"

#include <mio/mmap.hpp>

#include <imgui.h>

#include <gamecore/gc_app.h>
#include <gamecore/gc_world.h>
#include <gamecore/gc_net.h>

class WorldLoadSystem : public gc::System {
    bool m_loaded = false;

public:
    WorldLoadSystem(gc::World& world) : gc::System(world) {}

    void onUpdate([[maybe_unused]] gc::FrameState& frame_state) override
    {
        if (!m_loaded) {
            m_loaded = true;
        }
    }
};

void buildAndStartServer(gc::App& app, Options options)
{
    gc::World& world = app.world();
    world.registerSystem<WorldLoadSystem>();

    asio::ip::address addr = asio::ip::address_v4(); // default 0.0.0.0
    if (!options.bind_address.empty()) {
        asio::error_code ec{};
        addr = asio::ip::make_address(options.bind_address, ec);
        if (ec) {
            gc::abortGame("Failed to create IP address: {}", options.bind_address);
        }
    }

    asio::ip::udp::endpoint server_endpoint(addr, options.bind_port);

    if (!app.net().startServer(server_endpoint)) {
        gc::abortGame("Failed to start server");
    }

    app.run();

    app.net().stopServer();
}
