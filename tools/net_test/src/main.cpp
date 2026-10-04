//
// net_test.exe
//
// Tests the networking transport between separate processes. See README.
//

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_process.h>

#include <gamecore/gc_byte_reader.h>
#include <gamecore/gc_byte_writer.h>
#include <gamecore/gc_net.h>

using namespace gc::literals;

struct Options {
    std::string host{"127.0.0.1"};
    uint16_t port{46960};
    uint32_t clients{1};
    uint32_t messages{300};
    uint32_t timeout_seconds{90};
    bool strict{false};
    bool expect_no_server{false};
    bool shutdown_server{false};
    gc::NetSimConfig sim{};
};

template <typename... Args>
static void print(std::string_view role, std::format_string<Args...> fmt, Args&&... args)
{
    const std::string line = std::format("[{}] {}\n", role, std::format(fmt, std::forward<Args>(args)...));
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

static Options parseOptions(int argc, char* argv[], int first)
{
    Options options{};
    for (int i = first; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--strict") {
            options.strict = true;
            continue;
        }
        if (arg == "--expect-no-server") {
            options.expect_no_server = true;
            continue;
        }
        if (arg == "--shutdown-server") {
            options.shutdown_server = true;
            continue;
        }
        if (i + 1 >= argc) {
            print("net_test", "Missing value for option {}", arg);
            std::exit(EXIT_FAILURE);
        }
        const char* const value = argv[++i];
        if (arg == "--host") {
            options.host = value;
        }
        else if (arg == "--port") {
            options.port = static_cast<uint16_t>(std::atoi(value));
        }
        else if (arg == "--clients") {
            options.clients = static_cast<uint32_t>(std::atoi(value));
        }
        else if (arg == "--messages") {
            options.messages = static_cast<uint32_t>(std::atoi(value));
        }
        else if (arg == "--timeout") {
            options.timeout_seconds = static_cast<uint32_t>(std::atoi(value));
        }
        else if (arg == "--loss") {
            options.sim.loss_percent = static_cast<float>(std::atof(value));
            options.sim.enabled = true;
        }
        else if (arg == "--duplicate") {
            options.sim.duplicate_percent = static_cast<float>(std::atof(value));
            options.sim.enabled = true;
        }
        else if (arg == "--latency") {
            options.sim.latency_ms = static_cast<float>(std::atof(value));
            options.sim.enabled = true;
        }
        else if (arg == "--jitter") {
            options.sim.jitter_ms = static_cast<float>(std::atof(value));
            options.sim.enabled = true;
        }
        else {
            print("net_test", "Unknown option {}", arg);
            std::exit(EXIT_FAILURE);
        }
    }
    return options;
}

// A mix of small messages, messages that need a few fragments, and messages that need a lot of fragments
static size_t getTestMessageSize(uint32_t index)
{
    if (index % 50 == 7) {
        return 60000;
    }
    else if (index % 10 == 3) {
        return 3000 + index;
    }
    else {
        return sizeof(uint32_t) + (index * 37) % 500;
    }
}

static uint8_t getTestMessageByte(uint32_t index, size_t offset) { return static_cast<uint8_t>(index * 31 + offset * 7 + (offset >> 8)); }

static std::vector<uint8_t> makeTestMessage(uint32_t index, size_t size)
{
    std::vector<uint8_t> data(size);
    gc::ByteWriter writer(data);
    writer.writeU32(index);
    for (size_t i = sizeof(uint32_t); i < size; ++i) {
        data[i] = getTestMessageByte(index, i);
    }
    return data;
}

// Returns the index of the message if its contents are intact
static std::optional<uint32_t> verifyTestMessage(const std::vector<uint8_t>& data, bool reliable)
{
    if (data.size() < sizeof(uint32_t)) {
        return std::nullopt;
    }
    gc::ByteReader reader(data);
    const uint32_t index = reader.readU32();
    if (reliable && data.size() != getTestMessageSize(index)) {
        return std::nullopt;
    }
    for (size_t i = sizeof(uint32_t); i < data.size(); ++i) {
        if (data[i] != getTestMessageByte(index, i)) {
            return std::nullopt;
        }
    }
    return index;
}

static std::optional<asio::ip::udp::endpoint> makeEndpoint(const Options& options)
{
    asio::error_code ec{};
    const auto address = asio::ip::make_address(options.host, ec);
    if (ec) {
        return std::nullopt;
    }
    return asio::ip::udp::endpoint(address, options.port);
}

// Echoes every reliable message back to the client that sent it and checks that they arrive intact and in order.
static int runServer(const Options& options)
{
    constexpr std::string_view ROLE = "server";

    struct ClientState {
        uint32_t reliable_received{};
        uint32_t unreliable_received{};
        bool done{};
    };

    const auto endpoint = makeEndpoint(options);
    if (!endpoint) {
        print(ROLE, "Invalid address: {}", options.host);
        return EXIT_FAILURE;
    }

    gc::Net net{};
    net.setSimConfig(options.sim);
    if (!net.startServer(*endpoint)) {
        print(ROLE, "Failed to start");
        return EXIT_FAILURE;
    }

    std::unordered_map<gc::NetPeerId, ClientState> clients{};
    uint32_t finished_clients = 0;
    bool success = true;
    bool shutdown_requested = false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
    while (finished_clients < options.clients && !shutdown_requested) {
        if (std::chrono::steady_clock::now() > deadline) {
            print(ROLE, "FAIL: timed out waiting for {} clients to finish ({} did)", options.clients, finished_clients);
            success = false;
            break;
        }

        gc::NetEvent ev{};
        while (net.pollEvents(ev)) {
            ClientState& client = clients[ev.peer];
            switch (ev.kind) {
            case gc::NetEventKind::CONNECTED:
                print(ROLE, "client {} connected", ev.peer);
                break;
            case gc::NetEventKind::DISCONNECTED:
                print(ROLE, "client {} disconnected ({}): received {} reliable, {} unreliable", ev.peer, gc::netDisconnectReasonString(ev.reason),
                      client.reliable_received, client.unreliable_received);
                if (!client.done) {
                    print(ROLE, "FAIL: client {} disconnected before it finished", ev.peer);
                    success = false;
                }
                ++finished_clients;
                break;
            case gc::NetEventKind::MESSAGE:
                if (ev.type == "test_reliable"_name) {
                    const auto index = verifyTestMessage(ev.data, true);
                    if (index != client.reliable_received) {
                        print(ROLE, "FAIL: client {}: expected reliable message {}, got {}", ev.peer, client.reliable_received,
                              index ? std::to_string(*index) : "a corrupt message");
                        success = false;
                    }
                    ++client.reliable_received;
                    net.postEvent(ev, gc::NetDelivery::RELIABLE, ev.peer);
                }
                else if (ev.type == "test_unreliable"_name) {
                    if (!verifyTestMessage(ev.data, false)) {
                        print(ROLE, "FAIL: client {}: corrupt unreliable message", ev.peer);
                        success = false;
                    }
                    ++client.unreliable_received;
                }
                else if (ev.type == "shutdown"_name) {
                    // the same event that gc::App reacts to, so that this test can also be pointed at a real dedicated server
                    print(ROLE, "client {} asked the server to shut down", ev.peer);
                    shutdown_requested = true;
                }
                else if (ev.type == "test_done"_name && ev.data.size() == sizeof(uint32_t)) {
                    const uint32_t sent = gc::ByteReader(ev.data).readU32();
                    if (sent != client.reliable_received) {
                        print(ROLE, "FAIL: client {} sent {} reliable messages but {} were received before it finished", ev.peer, sent,
                              client.reliable_received);
                        success = false;
                    }
                    client.done = true;

                    gc::NetEvent ack{};
                    ack.type = "test_done_ack"_name;
                    ack.data.resize(2 * sizeof(uint32_t));
                    gc::ByteWriter writer(ack.data);
                    writer.writeU32(client.reliable_received);
                    writer.writeU32(client.unreliable_received);
                    net.postEvent(ack, gc::NetDelivery::RELIABLE, ev.peer);
                }
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    net.stopServer();
    print(ROLE, "{}", success ? "PASS" : "FAIL");
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int runClient(const Options& options)
{
    constexpr std::string_view ROLE = "client";
    constexpr uint32_t MESSAGES_PER_ITERATION = 4;

    const auto endpoint = makeEndpoint(options);
    if (!endpoint) {
        print(ROLE, "Invalid address: {}", options.host);
        return EXIT_FAILURE;
    }

    gc::Net net{};
    net.setSimConfig(options.sim);
    if (!net.connectToServer(*endpoint)) {
        print(ROLE, "Failed to start connecting");
        return EXIT_FAILURE;
    }

    bool connected = false;
    bool shutdown_sent = false;
    bool done_sent = false;
    bool done_acked = false;
    bool success = true;
    uint32_t sent = 0;
    uint32_t echoes_received = 0;
    uint32_t server_reliable_received = 0;
    uint32_t server_unreliable_received = 0;
    gc::NetConnectionStats stats{};

    const auto start_time = std::chrono::steady_clock::now();
    const auto deadline = start_time + std::chrono::seconds(options.timeout_seconds);
    bool running = true;
    while (running) {
        if (std::chrono::steady_clock::now() > deadline) {
            print(ROLE, "FAIL: timed out. sent {}/{}, {} echoes received", sent, options.messages, echoes_received);
            success = false;
            break;
        }

        gc::NetEvent ev{};
        while (net.pollEvents(ev)) {
            switch (ev.kind) {
            case gc::NetEventKind::CONNECTED:
                print(ROLE, "connected as client {}", net.getLocalPeerId());
                connected = true;
                break;
            case gc::NetEventKind::DISCONNECTED:
                if (options.expect_no_server && !connected && ev.reason == gc::NetDisconnectReason::CONNECT_FAILED) {
                    print(ROLE, "PASS: connection failed as expected ({})", gc::netDisconnectReasonString(ev.reason));
                    return EXIT_SUCCESS;
                }
                if (options.shutdown_server && shutdown_sent && ev.reason == gc::NetDisconnectReason::SERVER_SHUTDOWN) {
                    print(ROLE, "PASS: the server shut down and said so");
                    return EXIT_SUCCESS;
                }
                print(ROLE, "FAIL: disconnected ({})", gc::netDisconnectReasonString(ev.reason));
                return EXIT_FAILURE;
            case gc::NetEventKind::MESSAGE:
                if (ev.type == "test_reliable"_name) {
                    const auto index = verifyTestMessage(ev.data, true);
                    if (index != echoes_received) {
                        print(ROLE, "FAIL: expected echo of reliable message {}, got {}", echoes_received,
                              index ? std::to_string(*index) : "a corrupt message");
                        success = false;
                    }
                    ++echoes_received;
                }
                else if (ev.type == "test_done_ack"_name && ev.data.size() == 2 * sizeof(uint32_t)) {
                    gc::ByteReader reader(ev.data);
                    server_reliable_received = reader.readU32();
                    server_unreliable_received = reader.readU32();
                    done_acked = true;
                }
                break;
            }
        }

        if (connected && options.shutdown_server) {
            if (!shutdown_sent) {
                gc::NetEvent shutdown{};
                shutdown.type = "shutdown"_name;
                net.postEvent(shutdown, gc::NetDelivery::RELIABLE);
                shutdown_sent = true;
            }
        }
        else if (connected) {
            for (uint32_t i = 0; i < MESSAGES_PER_ITERATION && sent < options.messages; ++i, ++sent) {
                gc::NetEvent reliable{};
                reliable.type = "test_reliable"_name;
                reliable.data = makeTestMessage(sent, getTestMessageSize(sent));
                net.postEvent(reliable, gc::NetDelivery::RELIABLE);

                gc::NetEvent unreliable{};
                unreliable.type = "test_unreliable"_name;
                unreliable.data = makeTestMessage(sent, 32);
                net.postEvent(unreliable, gc::NetDelivery::UNRELIABLE);
            }
            if (sent == options.messages && !done_sent) {
                gc::NetEvent done{};
                done.type = "test_done"_name;
                done.data.resize(sizeof(uint32_t));
                gc::ByteWriter(done.data).writeU32(sent);
                net.postEvent(done, gc::NetDelivery::RELIABLE);
                done_sent = true;
            }
            if (const auto peers = net.getPeers(); !peers.empty()) {
                stats = peers.front().stats;
            }
            // The acknowledgement is sent after the last echo, and reliable messages arrive in order
            running = !done_acked;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    net.disconnectFromServer();

    if (options.expect_no_server) {
        print(ROLE, "FAIL: expected the connection to fail");
        return EXIT_FAILURE;
    }
    if (options.shutdown_server) {
        print(ROLE, "FAIL: the server never said it was shutting down");
        return EXIT_FAILURE;
    }

    if (done_acked) {
        if (echoes_received != options.messages) {
            print(ROLE, "FAIL: only {}/{} echoes were received before the server acknowledged the end of the test", echoes_received, options.messages);
            success = false;
        }
        if (server_reliable_received != options.messages) {
            print(ROLE, "FAIL: server received {}/{} reliable messages", server_reliable_received, options.messages);
            success = false;
        }
        if (server_unreliable_received > options.messages + stats.packets_sent) {
            print(ROLE, "FAIL: server received more unreliable messages ({}) than can be explained by duplication", server_unreliable_received);
            success = false;
        }
        if (options.strict && server_unreliable_received != options.messages) {
            print(ROLE, "FAIL: server received {}/{} unreliable messages", server_unreliable_received, options.messages);
            success = false;
        }
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    print(ROLE, "{} in {:.2f} s: reliable {}/{} delivered and echoed, unreliable {}/{} delivered", success ? "PASS" : "FAIL", seconds, echoes_received,
          options.messages, server_unreliable_received, options.messages);
    print(ROLE, "  rtt {:.1f} ms (smoothed {:.1f} ms), loss {:.1f} %, packets out/in/lost {}/{}/{}, fragments resent {}, bytes out/in {}/{}", stats.rtt_ms,
          stats.smoothed_rtt_ms, stats.packet_loss * 100.0f, stats.packets_sent, stats.packets_received, stats.packets_lost, stats.reliable_resent,
          stats.bytes_sent, stats.bytes_received);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}

static SDL_Process* spawn(const std::string& executable, const std::vector<std::string>& args)
{
    std::vector<const char*> argv{};
    argv.push_back(executable.c_str());
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);
    SDL_Process* const process = SDL_CreateProcess(argv.data(), false); // inherits stdout
    if (!process) {
        print("net_test", "Failed to start process: {}", SDL_GetError());
    }
    return process;
}

// returns true if the process exited successfully
static bool waitFor(SDL_Process* process)
{
    if (!process) {
        return false;
    }
    int exit_code = EXIT_FAILURE;
    SDL_WaitProcess(process, true, &exit_code);
    SDL_DestroyProcess(process);
    return exit_code == EXIT_SUCCESS;
}

static int runAllScenarios(const char* argv0)
{
    constexpr std::string_view ROLE = "net_test";
    constexpr uint32_t NUM_CLIENTS = 2;
    constexpr uint16_t BASE_PORT = 46960;

    struct Scenario {
        const char* name;
        gc::NetSimConfig sim; // applied to the clients, which covers both directions of the link
        bool strict;
    };
    const std::vector<Scenario> scenarios{
        {"perfect link", {}, true},
        {"bad link: 20% loss, 5% duplication, 40 +/- 30 ms latency", {true, 20.0f, 5.0f, 40.0f, 30.0f}, false},
        {"terrible link: 40% loss, 10% duplication, 100 +/- 80 ms latency", {true, 40.0f, 10.0f, 100.0f, 80.0f}, false},
    };

    std::filesystem::path executable_path = std::filesystem::path(SDL_GetBasePath()) / std::filesystem::path(argv0).filename();
#ifdef _WIN32
    executable_path.replace_extension(".exe");
#endif
    const std::string executable = executable_path.string();

    int failures = 0;
    uint16_t port = BASE_PORT;

    for (const Scenario& scenario : scenarios) {
        print(ROLE, "=== {} ===", scenario.name);
        const std::string port_string = std::to_string(port++);

        SDL_Process* const server = spawn(executable, {"server", "--port", port_string, "--clients", std::to_string(NUM_CLIENTS)});

        std::vector<std::string> client_args{"client", "--port", port_string};
        if (scenario.strict) {
            client_args.push_back("--strict");
        }
        if (scenario.sim.enabled) {
            client_args.insert(client_args.end(), {"--loss", std::to_string(scenario.sim.loss_percent), "--duplicate", std::to_string(scenario.sim.duplicate_percent),
                                                   "--latency", std::to_string(scenario.sim.latency_ms), "--jitter", std::to_string(scenario.sim.jitter_ms)});
        }
        std::vector<SDL_Process*> clients{};
        for (uint32_t i = 0; i < NUM_CLIENTS; ++i) {
            clients.push_back(spawn(executable, client_args));
        }

        bool passed = true;
        for (SDL_Process* const client : clients) {
            passed &= waitFor(client);
        }
        if (!passed && server) {
            SDL_KillProcess(server, true); // it would otherwise wait for clients that are never going to finish
        }
        passed &= waitFor(server);

        print(ROLE, "=== {}: {} ===", scenario.name, passed ? "PASS" : "FAIL");
        failures += passed ? 0 : 1;
    }

    {
        print(ROLE, "=== server shuts down while a client is connected ===");
        const std::string port_string = std::to_string(port++);
        SDL_Process* const server = spawn(executable, {"server", "--port", port_string});
        bool passed = waitFor(spawn(executable, {"client", "--port", port_string, "--shutdown-server", "--timeout", "20"}));
        if (!passed && server) {
            SDL_KillProcess(server, true);
        }
        passed &= waitFor(server);
        print(ROLE, "=== server shuts down while a client is connected: {} ===", passed ? "PASS" : "FAIL");
        failures += passed ? 0 : 1;
    }

    {
        print(ROLE, "=== no server listening ===");
        const bool passed = waitFor(spawn(executable, {"client", "--port", std::to_string(port), "--expect-no-server"}));
        print(ROLE, "=== no server listening: {} ===", passed ? "PASS" : "FAIL");
        failures += passed ? 0 : 1;
    }

    print(ROLE, "{}", failures == 0 ? "ALL SCENARIOS PASSED" : "SOME SCENARIOS FAILED");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        return runAllScenarios(argv[0]);
    }

    const std::string_view mode(argv[1]);
    if (mode == "server") {
        return runServer(parseOptions(argc, argv, 2));
    }
    else if (mode == "client") {
        return runClient(parseOptions(argc, argv, 2));
    }
    else {
        print("net_test", "usage: net_test [server|client] [options]. See README for options.");
        return EXIT_FAILURE;
    }
}
