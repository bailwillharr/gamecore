#include "gamecore/gc_net_ui.h"

#include <array>

#include <imgui.h>

#include "gamecore/gc_net.h"
#include "gamecore/gc_net_client.h"

namespace gc {

void renderNetUI(Net& net)
{
    if (ImGui::Begin("Network")) {
        const auto mode = net.getMode();
        const char* mode_str{};
        switch (mode) {
        case NetMode::DISCONNECTED:
            mode_str = "disconnected";
            break;
        case NetMode::SERVER:
            mode_str = "server";
            break;
        case NetMode::CLIENT:
            mode_str = "client";
            break;
        default:
            mode_str = "(invalid)";
        }
        ImGui::Text("Mode: %s", mode_str);

        switch (mode) {
        case NetMode::DISCONNECTED: {
            static std::array<char, 64> buf{};
            static int port{};
            ImGui::InputTextWithHint("Server IP", "127.0.0.1", buf.data(), buf.size());
            ImGui::InputInt("Server Port", &port);

            asio::ip::address address{};
            asio::error_code ec{};
            if (buf[0]) {
                address = asio::ip::make_address(buf.data(), ec);
            }
            else {
                address = asio::ip::make_address("127.0.0.1", ec);
            }
            if (ec) {
                break;
            }

            if (port < 0 || port > 65535) {
                break;
            }

            asio::ip::udp::endpoint endpoint(address, port);

            if (ImGui::Button("Start Server")) {
                net.startServer(endpoint);
            }
            if (ImGui::Button("Conncet To Server")) {
                net.connectToServer(endpoint);
            }
        } break;
        case NetMode::SERVER: {
            ImGui::Text("Running: %d", net.isServerRunning());
            if (ImGui::Button("Stop Server")) {
                net.stopServer();
            }
        } break;
        case NetMode::CLIENT: {
            const char* status_str{};
            switch (net.getClientState()) {
            case NetClientState::DISCONNECTED:
                status_str = "disconnected";
                break;
            case NetClientState::CONNECTING:
                status_str = "connecting";
                break;
            case NetClientState::CONNECTED:
                status_str = "connected";
                break;
            default:
                status_str = "(invalid)";
            }
            ImGui::Text("Status: %s", status_str);
            if (ImGui::Button("Disconnect")) {
                net.disconnectFromServer();
            }
        } break;
        }
    }
    ImGui::End();
}

} // namespace gc
