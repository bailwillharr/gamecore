#include <SDL3/SDL_main.h>

#include <span>
#include <string_view>
#include <optional>

#include <gctemplates/gct_static_vector.h>

#include <gamecore/gc_app.h>
#include <gamecore/gc_logger.h>

#include "game.h"

Options parseCmdLine(std::span<const char* const> args)
{
    Options result{};
    for (auto arg : args) {
        std::string_view sv(arg);
        if (sv.starts_with("syncmode=")) {
            int value{};
            const char* const first = sv.data() + 9;
            const char* const last = sv.data() + sv.size();
            if (std::from_chars(first, last, value).ptr == last) {
                if (value < 4) {
                    result.render_sync_mode = value;
                }
            }
        }
    }
    return result;
}

int main(int argc, char* argv[])
{

    auto options = parseCmdLine(std::span(argv + 1, argc - 1));

    gc::AppInitOptions init_options{};
    init_options.name = "gamecore_template";
    init_options.author = "bailwillharr";
    init_options.version = "v0.0.0";

    gc::App::initialise(init_options);

    buildAndStartGame(gc::App::instance(), options);

    gc::App::shutdown();

    // Critical errors in the engine call gc::abortGame() therefore main() can always return 0
    return 0;
}
