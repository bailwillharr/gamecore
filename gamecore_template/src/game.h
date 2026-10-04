#pragma once

#include <cstdint>

#include <optional>
#include <span>
#include <string>

#include <gamecore/gc_net_common.h>

namespace gc {
class App; // forward-dec
}

enum class GameMode {
    OFFLINE,          // single player. A server can still be started (or joined) later from the Network debug window
    HOST,             // play, and let other players join
    DEDICATED_SERVER, // no window and no local player
    CLIENT,           // join a server
};

struct Options {
    std::optional<int> render_sync_mode{};
    GameMode mode{GameMode::OFFLINE};
    std::string address{"127.0.0.1"}; // CLIENT: the server to join
    std::string bind_address{};       // HOST, DEDICATED_SERVER: the address to listen on. Empty for all addresses
    uint16_t port{6969};
    bool bot{false};             // no window. The local player is controlled by the computer
    bool test{false};            // bot only: check that replication is working, then exit
    float test_timeout{40.0f};   // seconds
    float test_freeze_time{0.0f}; // seconds. If not zero: stop changing anything after this long, then report the state digest
    bool exit_when_empty{false}; // server only: exit once every client that joined has left
    std::string log_file{};      // overrides the default log file, so that several instances don't share one
    gc::NetSimConfig sim{};
};

// Command line:
//   syncmode=N                 see gc::RenderSyncMode
//   --host [port]              play and accept connections
//   --server [port]            dedicated server
//   --connect address[:port]   join a server
//   --port port
//   --bind address             address for --host / --server to listen on
//   --bot                      run without a window, as a computer controlled player (use with --connect or --host)
//   --test                     with --bot: exit with code 0 once replication has been seen to work, or 1 after --test-timeout seconds
//   --test-timeout seconds
//   --test-freeze seconds      everything stops moving after this long. 5 seconds later the host logs a digest of all replicated
//                              state (and exits, if it's a client). Every host of the same game should log the same digest.
//   --exit-when-empty          with --server: exit once all clients have left
//   --log file
//   --sim-loss percent, --sim-duplicate percent, --sim-latency ms, --sim-jitter ms     simulate a bad connection
Options parseCommandLine(std::span<const char* const> args);

bool isHeadless(const Options& options);

// Returns the process exit code
int buildAndStartGame(gc::App& app, const Options& options);
