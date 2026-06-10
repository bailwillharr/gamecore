#include "gamecore/gc_net_ui.h"

#include <cinttypes>

#include <array>

#include <imgui.h>

#include "gamecore/gc_assert.h"
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
            static int port{NET_DEFAULT_SERVER_PORT};
            ImGui::InputTextWithHint("Server Address", "0.0.0.0", buf.data(), buf.size());
            ImGui::InputInt("Server Port", &port);

            if (port < 0 || port > 65535) {
                break;
            }

            asio::ip::address address{};
            bool use_resolver = false;
            asio::error_code ec{};
            if (buf[0]) {
                address = asio::ip::make_address(buf.data(), ec);
                if (ec == asio::error::invalid_argument) {
                    // try to resolve as a domain name instead
                    use_resolver = true;
                }
                else if (ec) {
                    break;
                }
            }
            else {
                address = asio::ip::make_address("127.0.0.1", ec);
                GC_ASSERT(!ec);
            }

            asio::ip::udp::endpoint endpoint(address, asio::ip::port_type(port));

            if (!use_resolver) {
                if (ImGui::Button("Start Server")) {
                    net.startServer(endpoint);
                }
            }
            if (ImGui::Button("Connect To Server")) {
                if (use_resolver) {
                    const auto endpoint_opt = net.resolve(std::string_view(buf.data()), std::to_string(port));
                    if (!endpoint_opt) {
                        GC_ERROR("Failed to resolve: {}", buf.data());
                        break;
                    }
                    endpoint = *endpoint_opt;
                }
                net.connectToServer(endpoint);
            }
        } break;
        case NetMode::SERVER: {
            if (ImGui::Button("Stop Server")) {
                net.stopServer();
            }
            if (ImGui::Button("Send shutdown command to clients")) {
                net.postEvent(NetEvent{Name("shutdown"), {}});
            }
            const auto server_addr = net.getServerEndpoint();
            ImGui::Text("Address: %s:%d", server_addr.address().to_string().c_str(), server_addr.port());
            ImGui::Text("%u clients:", net.getRemoteCount());
            const auto client_sessions = net.getRemoteSessions();
            for (const NetSessionToken session : client_sessions) {
                ImGui::Text("  %16" PRIX64, session);
            }
        } break;
        case NetMode::CLIENT: {
            const char* status_str{};
            switch (net.getClientConnectionStatus()) {
            case NetClientConnectionStatus::DISCONNECTED:
                status_str = "disconnected";
                break;
            case NetClientConnectionStatus::CONNECTING:
                status_str = "connecting";
                break;
            case NetClientConnectionStatus::CONNECTED:
                status_str = "connected";
                break;
            default:
                status_str = "(invalid)";
            }
            ImGui::Text("Status: %s", status_str);
            if (ImGui::Button("Disconnect")) {
                net.disconnectFromServer();
            }
            if (ImGui::Button("Send shutdown command to server")) {
                net.postEvent(NetEvent{Name("shutdown"), {}});
            }
        } break;
        }
    }
    ImGui::End();
}

} // namespace gc
