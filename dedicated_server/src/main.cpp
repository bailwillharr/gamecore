#include <SDL3/SDL_main.h>

#include <span>
#include <string_view>
#include <vector>

#include <gclog/gclog.h>

#include <gamecore/gc_app.h>

// The dedicated server runs the same game as gamecore_template, without a window or a local player.
#include <game.h>

// Command line: ./dedicated_server [port [address]] [options]
// The options are the ones understood by gamecore_template (see game.h). e.g. --exit-when-empty, --log file
int main(int argc, char* argv[])
{
    // The port and address can be given without naming them. Turn them into the equivalent options.
    std::vector<const char*> args{};
    int next_arg = 1;
    if (next_arg < argc && !std::string_view(argv[next_arg]).starts_with("--")) {
        args.push_back("--port");
        args.push_back(argv[next_arg++]);
        if (next_arg < argc && !std::string_view(argv[next_arg]).starts_with("--")) {
            args.push_back("--bind");
            args.push_back(argv[next_arg++]);
        }
    }
    for (; next_arg < argc; ++next_arg) {
        args.push_back(argv[next_arg]);
    }

    Options options = parseCommandLine(args);
    options.mode = GameMode::DEDICATED_SERVER;
    options.bot = false;

    if (!options.log_file.empty()) {
        gclog::Logger::instance().setLogFile(options.log_file);
    }

    gc::AppInitOptions init_options{};
    init_options.name = "dedicated_server";
    init_options.author = "bailwillharr";
    init_options.version = "v0.0.0";
    init_options.headless = true;

    gc::App::initialise(init_options);

    const int exit_code = buildAndStartGame(gc::App::instance(), options);

    gc::App::shutdown();

    return exit_code;
}
