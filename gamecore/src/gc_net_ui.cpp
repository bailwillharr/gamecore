#include "gamecore/gc_net_ui.h"

#include <cinttypes>

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>

#include <imgui.h>

#include "gamecore/gc_assert.h"
#include "gamecore/gc_net.h"
#include "gamecore/gc_net_client.h"

namespace gc {

namespace {

// Recent samples of a peer's connection, for plotting
struct PeerHistory {
    static constexpr int NUM_SAMPLES = 200;
    static constexpr double SAMPLE_PERIOD = 0.05; // seconds

    std::array<float, NUM_SAMPLES> rtt_ms{};
    std::array<float, NUM_SAMPLES> loss_percent{};
    std::array<float, NUM_SAMPLES> send_kbps{};
    std::array<float, NUM_SAMPLES> receive_kbps{};
    int offset{};
    double last_sample_time{};
    bool seen{}; // used to forget peers that have gone
};

} // namespace

static constexpr float BYTES_PER_SEC_TO_KBPS = 8.0f / 1000.0f;

static void recordHistory(PeerHistory& history, const NetConnectionStats& stats, double now)
{
    history.seen = true;
    if (now - history.last_sample_time < PeerHistory::SAMPLE_PERIOD) {
        return;
    }
    history.last_sample_time = now;
    history.rtt_ms[history.offset] = stats.rtt_ms;
    history.loss_percent[history.offset] = stats.packet_loss * 100.0f;
    history.send_kbps[history.offset] = stats.send_rate * BYTES_PER_SEC_TO_KBPS;
    history.receive_kbps[history.offset] = stats.receive_rate * BYTES_PER_SEC_TO_KBPS;
    history.offset = (history.offset + 1) % PeerHistory::NUM_SAMPLES;
}

static void renderPlot(const char* label, const std::array<float, PeerHistory::NUM_SAMPLES>& samples, int offset, const char* overlay, float min_scale_max)
{
    const float scale_max = std::max(min_scale_max, *std::max_element(samples.begin(), samples.end()) * 1.1f);
    ImGui::PlotLines(label, samples.data(), static_cast<int>(samples.size()), offset, overlay, 0.0f, scale_max, ImVec2(0.0f, 48.0f));
}

static void renderStatRow(const char* name, const std::string& value)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.c_str());
}

static void renderPeerDetails(const NetPeerInfo& peer, const PeerHistory& history)
{
    const NetConnectionStats& stats = peer.stats;

    renderPlot("RTT", history.rtt_ms, history.offset, std::format("{:.1f} ms", stats.rtt_ms).c_str(), 10.0f);
    renderPlot("Loss", history.loss_percent, history.offset, std::format("{:.1f} %", stats.packet_loss * 100.0f).c_str(), 5.0f);
    renderPlot("Out", history.send_kbps, history.offset, std::format("{:.1f} kbit/s", stats.send_rate * BYTES_PER_SEC_TO_KBPS).c_str(), 10.0f);
    renderPlot("In", history.receive_kbps, history.offset, std::format("{:.1f} kbit/s", stats.receive_rate * BYTES_PER_SEC_TO_KBPS).c_str(), 10.0f);

    if (ImGui::BeginTable("net_peer_stats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        renderStatRow("Endpoint", std::format("{}", peer.endpoint));
        renderStatRow("RTT (latest / smoothed)", std::format("{:.1f} ms / {:.1f} ms", stats.rtt_ms, stats.smoothed_rtt_ms));
        renderStatRow("Retransmit timeout", std::format("{:.1f} ms", stats.rto_ms));
        renderStatRow("Last received", std::format("{:.0f} ms ago", stats.idle_ms));
        renderStatRow("Packets (out / in)", std::format("{} / {}", stats.packets_sent, stats.packets_received));
        renderStatRow("Packets (acked / lost)", std::format("{} / {}", stats.packets_acked, stats.packets_lost));
        renderStatRow("Packets ignored (duplicate / malformed)", std::format("{} / {}", stats.packets_duplicate, stats.packets_malformed));
        renderStatRow("Bytes (out / in)", std::format("{} / {}", stats.bytes_sent, stats.bytes_received));
        renderStatRow("Reliable messages (out / in)", std::format("{} / {}", stats.reliable_sent, stats.reliable_received));
        renderStatRow("Reliable fragments resent", std::format("{}", stats.reliable_resent));
        renderStatRow("Reliable fragments queued", std::format("{}", stats.reliable_queue));
        renderStatRow("Congestion window", std::format("{:.0f} packets", stats.congestion_window));
        renderStatRow("Unreliable messages (out / in)", std::format("{} / {}", stats.unreliable_sent, stats.unreliable_received));
        renderStatRow("Unreliable messages (queued / dropped)", std::format("{} / {}", stats.unreliable_queue, stats.unreliable_dropped));
        ImGui::EndTable();
    }
}

static void renderConnectControls(Net& net)
{
    static std::array<char, 64> s_address{};
    static int s_port{NET_DEFAULT_SERVER_PORT};
    ImGui::InputTextWithHint("Server Address", "127.0.0.1", s_address.data(), s_address.size());
    ImGui::InputInt("Server Port", &s_port);

    if (const auto reason = net.getLastDisconnectReason(); reason != NetDisconnectReason::NONE) {
        ImGui::Text("Last disconnect: %s", netDisconnectReasonString(reason));
    }

    if (s_port < 0 || s_port > 65535) {
        return;
    }

    asio::ip::address address{};
    bool use_resolver = false;
    asio::error_code ec{};
    if (s_address[0]) {
        address = asio::ip::make_address(s_address.data(), ec);
        if (ec == asio::error::invalid_argument) {
            // try to resolve as a domain name instead
            use_resolver = true;
        }
        else if (ec) {
            return;
        }
    }
    else {
        address = asio::ip::make_address("127.0.0.1", ec);
        GC_ASSERT(!ec);
    }

    asio::ip::udp::endpoint endpoint(address, asio::ip::port_type(s_port));

    if (!use_resolver) {
        if (ImGui::Button("Start Server")) {
            net.startServer(endpoint);
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Connect To Server")) {
        if (use_resolver) {
            const auto endpoint_opt = net.resolve(std::string_view(s_address.data()), std::to_string(s_port));
            if (!endpoint_opt) {
                GC_ERROR("Failed to resolve: {}", s_address.data());
                return;
            }
            endpoint = *endpoint_opt;
        }
        net.connectToServer(endpoint);
    }
}

// Returns the peer whose details should be shown
static NetPeerId renderPeerTable(Net& net, const std::vector<NetPeerInfo>& peers, NetPeerId selected_peer)
{
    constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("net_peers", 8, FLAGS)) {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Endpoint");
        ImGui::TableSetupColumn("RTT");
        ImGui::TableSetupColumn("Loss");
        ImGui::TableSetupColumn("Out");
        ImGui::TableSetupColumn("In");
        ImGui::TableSetupColumn("Queue");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const NetPeerInfo& peer : peers) {
            ImGui::PushID(static_cast<int>(peer.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // the whole row is clickable, apart from the kick button
            constexpr ImGuiSelectableFlags SELECTABLE_FLAGS = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;
            if (ImGui::Selectable(std::to_string(peer.id).c_str(), peer.id == selected_peer, SELECTABLE_FLAGS)) {
                selected_peer = peer.id;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(std::format("{}", peer.endpoint).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.1f ms", peer.stats.smoothed_rtt_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f %%", peer.stats.packet_loss * 100.0f);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f kbit/s", peer.stats.send_rate * BYTES_PER_SEC_TO_KBPS);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f kbit/s", peer.stats.receive_rate * BYTES_PER_SEC_TO_KBPS);
            ImGui::TableNextColumn();
            ImGui::Text("%u", peer.stats.reliable_queue);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Kick")) {
                net.disconnectPeer(peer.id);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return selected_peer;
}

static void renderSimulatorControls(Net& net)
{
    NetSimConfig config = net.getSimConfig();
    bool changed = false;
    changed |= ImGui::Checkbox("Enabled", &config.enabled);
    changed |= ImGui::SliderFloat("Loss", &config.loss_percent, 0.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Duplication", &config.duplicate_percent, 0.0f, 100.0f, "%.0f %%", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Latency", &config.latency_ms, 0.0f, 1000.0f, "%.0f ms", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Jitter", &config.jitter_ms, 0.0f, 500.0f, "+/- %.0f ms", ImGuiSliderFlags_AlwaysClamp);
    if (changed) {
        net.setSimConfig(config);
    }
    ImGui::TextWrapped("Applies to every packet this process sends and receives, so the round trip time grows by twice the latency.");
}

static void renderTestControls(Net& net, NetMode mode)
{
    static int s_message_size{4096};
    static int s_burst_count{100};

    ImGui::SliderInt("Message size", &s_message_size, 0, static_cast<int>(NET_MAX_MESSAGE_SIZE), "%d bytes", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::Button("Send reliable message")) {
        NetEvent ev{};
        ev.type = Name("net_ui_test");
        ev.data.resize(static_cast<size_t>(s_message_size));
        net.postEvent(ev, NetDelivery::RELIABLE);
    }

    ImGui::SliderInt("Burst count", &s_burst_count, 1, 1000, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::Button("Send burst of unreliable messages")) {
        NetEvent ev{};
        ev.type = Name("net_ui_test");
        ev.data.resize(64);
        for (int i = 0; i < s_burst_count; ++i) {
            net.postEvent(ev, NetDelivery::UNRELIABLE);
        }
    }

    if (ImGui::Button(mode == NetMode::SERVER ? "Send shutdown command to clients" : "Send shutdown command to server")) {
        net.postEvent(NetEvent{.type = Name("shutdown")});
    }
}

void renderNetUI(Net& net, bool show)
{
    static std::unordered_map<NetPeerId, PeerHistory> s_histories{};
    static NetPeerId s_selected_peer{NET_PEER_NONE};

    // Histories are recorded even when the window is hidden or collapsed so that the plots have no gaps
    const NetMode mode = net.getMode();
    const std::vector<NetPeerInfo> peers = net.getPeers();
    {
        const double now = ImGui::GetTime();
        for (auto& [id, history] : s_histories) {
            history.seen = false;
        }
        for (const NetPeerInfo& peer : peers) {
            recordHistory(s_histories[peer.id], peer.stats, now);
        }
        std::erase_if(s_histories, [](const auto& entry) { return !entry.second.seen; });
    }

    if (!show) {
        return;
    }

    ImGui::SetNextWindowPos(ImVec2(20.0f, 140.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Network")) {
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

        const NetPeerInfo* details_peer = nullptr;

        switch (mode) {
        case NetMode::DISCONNECTED: {
            renderConnectControls(net);
        } break;
        case NetMode::SERVER: {
            const auto server_addr = net.getServerEndpoint();
            ImGui::Text("Listening on: %s", std::format("{}", server_addr).c_str());
            if (ImGui::Button("Stop Server")) {
                net.stopServer();
                break;
            }
            ImGui::Text("%u clients:", static_cast<unsigned>(peers.size()));
            s_selected_peer = renderPeerTable(net, peers, s_selected_peer);
            for (const NetPeerInfo& peer : peers) {
                if (peer.id == s_selected_peer) {
                    details_peer = &peer;
                }
            }
            if (!details_peer && !peers.empty()) {
                details_peer = &peers.front();
                s_selected_peer = details_peer->id;
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
            ImGui::Text("Server: %s", std::format("{}", net.getServerEndpoint()).c_str());
            ImGui::Text("Local peer ID: %u", net.getLocalPeerId());
            if (ImGui::Button("Disconnect")) {
                net.disconnectFromServer();
                break;
            }
            if (!peers.empty()) {
                details_peer = &peers.front();
            }
        } break;
        }

        if (details_peer) {
            const std::string header = (mode == NetMode::SERVER) ? std::format("Client {}###net_details", details_peer->id) : "Connection###net_details";
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                renderPeerDetails(*details_peer, s_histories[details_peer->id]);
            }
        }

        if (ImGui::CollapsingHeader("Link simulator")) {
            ImGui::PushID("net_sim");
            renderSimulatorControls(net);
            ImGui::PopID();
        }

        // re-query the mode as the buttons above can change it
        if (net.getMode() != NetMode::DISCONNECTED && ImGui::CollapsingHeader("Test traffic")) {
            ImGui::PushID("net_test");
            renderTestControls(net, net.getMode());
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace gc
