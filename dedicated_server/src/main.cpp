#include <SDL3/SDL_main.h>

#include <span>
#include <string_view>

#include <gctemplates/gct_static_vector.h>

#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>
#include <gamecore/gc_net.h>

#include "server.h"

Options parseCmdLine(std::span<const char* const> args)
{
    Options result{};
    result.bind_port = gc::NET_DEFAULT_SERVER_PORT;
    if (args.size() >= 1) {
        std::string_view port_string(args[0]);
        uint16_t port{};
        auto [ptr, ec] = std::from_chars(port_string.data(), port_string.data() + port_string.size(), port);
        if (ptr != port_string.data() + port_string.size()) {
            GC_ERROR("Failed to parse port cmd line argument");
        }
        else {
            result.bind_port = port;
        }
    }
    if (args.size() >= 2) {
        result.bind_address = args[1];
    }
    return result;
}

// Command line: ./dedicated_server [port [address]]
int main(int argc, char* argv[])
{

    auto options = parseCmdLine(std::span(argv + 1, argc - 1));

    gc::AppInitOptions init_options{};
    init_options.name = "gamecore_template";
    init_options.author = "bailwillharr";
    init_options.version = "v0.0.0";
    init_options.headless = true;

    gc::App::initialise(init_options);

    buildAndStartServer(gc::App::instance(), options);

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
